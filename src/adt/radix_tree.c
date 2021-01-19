#include <skp/adt/radix_tree.h>
#include <skp/process/thread.h>
#include <skp/mm/slab.h>

#define RADIX_TREE_MAP_SHIFT 6
#define RADIX_TREE_MAP_SIZE	(1UL << RADIX_TREE_MAP_SHIFT)// 64
#define RADIX_TREE_MAP_MASK	(RADIX_TREE_MAP_SIZE-1) // 0x3f

#define RADIX_TREE_TAG_LONGS	\
	((RADIX_TREE_MAP_SIZE + BITS_PER_LONG - 1) / BITS_PER_LONG)

struct radix_tree_node {
	unsigned int count;/**< 使用槽位的个数*/
	void *slots[RADIX_TREE_MAP_SIZE];//64个槽位，要么是外部对象（叶子节点），要么是radix_tree_node(非叶子节点）
	unsigned long	tags[RADIX_TREE_TAGS][RADIX_TREE_TAG_LONGS];/**<
		* (2 * 64 bits)
		* 1. 第一维下标 0 PAGECACHE_TAG_DIRTY 表示子树中有否脏页
		* 2. 第一维下标 1 PAGECACHE_TAG_WRITEBACK 表示树中有否回写页
		* 3. 第二维下标 对应每个槽位的位图 */
};
/*
 * node--->+--------+
 *         |0 |1 |2 |
 *         +--------+
 *          ^
 *          |
 * slot--+--+
 *       |
 *       |
 *  +--------+
 *  |0 |1 |2 |-----> current path
 *  +--------+
 */
struct radix_tree_path {
	struct radix_tree_node *parent/**<当前路径（层数）的所在父亲节点*/,
	**slot/**<当前路径的所在父亲节点中槽位的指针的指针*/;
	int offset;/**<所在父亲节点槽位数组中的偏移*/
};

#define RADIX_TREE_INDEX_BITS  (8U /* CHAR_BIT */ * sizeof(uint64_t))// 64
#define RADIX_TREE_MAX_PATH (RADIX_TREE_INDEX_BITS/RADIX_TREE_MAP_SHIFT + 2) // 12，多2个是首尾边界 0 和 ~0UL

static bool radix_tree_up = false;
static uint64_t height_to_maxindex[RADIX_TREE_MAX_PATH];

static inline uint64_t __maxindex(uint32_t height)
{
	/*获取 位移 倍数
	 * 每一层 （高度）能表示的索引大小为
	 * (1 << radix_tree_map_shift) ^ h
	 * 这里就是要求取边界序列：0, 64, 64*64(4096), 64*64*64(262114), 64*64*64*64...
	 * 每个数减 1
	 * 0, 63, 4095, 262113, ... , 0xffffffff
	 */
	uint32_t tmp = height * RADIX_TREE_MAP_SHIFT;
	/*获取与位移倍数匹配的 索引边界
	 * 将全为1的UL值右移剩余位数
	 */
	uint64_t index = (~0ULL >> ((RADIX_TREE_INDEX_BITS - 1) - tmp)) >> 1;

	if (tmp >= RADIX_TREE_INDEX_BITS)
		index = ~0ULL;
	return index;
}

static inline void rdtree_init_maxindex(void)
{
	for (uint32_t i = 0; i < ARRAY_SIZE(height_to_maxindex); i++)
		height_to_maxindex[i] = __maxindex(i);
}

/*
 * Per-cpu pool of preloaded nodes
 */
#define NODE_CACAH_MAX (RADIX_TREE_MAX_PATH * 2)

struct preload_tls {
	int nr;
	struct list_head head;
};

#ifdef UMALLOC_MANGLE
/* Radix tree node cache. */
static umem_cache_t *node_allocator = NULL;
#endif

static __thread struct preload_tls *preload_tls = NULL;

#ifdef DEBUG
# define RT_BUG_ON(x) BUG_ON((x))
#else
# define RT_BUG_ON(x) (x)
#endif

static void __preload_reclaim(void *ptr)
{
	struct list_head *node;
	struct preload_tls *tls = ptr;

	if (skp_unlikely(!tls))
		return;

	while (tls->nr) {
		RT_BUG_ON(list_empty(&tls->head));
		tls->nr--;
		node = tls->head.next;
		list_del(node);
		tls_free(node);
	}
}

static void preload_reclaim(void *ptr)
{
	__preload_reclaim(ptr);
	tls_free(ptr);
}

static inline void preload_init(void)
{
	struct preload_tls *tls;
#ifdef UMALLOC_MANGLE
	struct umem_cache_s *cache = node_allocator;

	if (skp_likely(READ_ONCE(cache)))
		goto done;

	big_lock();
	if (skp_likely(!node_allocator)) {
		cache = umem_cache_create("radix tree", sizeof(struct radix_tree_node),
						0, SLAB_RECLAIM|SLAB_HWCACHE_ALIGN);
		BUG_ON(!cache);
		smp_wmb();
		WRITE_ONCE(node_allocator, cache);
	}
	big_unlock();

done:
#endif
	tls = preload_tls;
	if (skp_likely(tls))
		return;

	tls = malloc(sizeof(*tls));
	BUG_ON(!tls);

	tls->nr = 0;
	INIT_LIST_HEAD(&tls->head);

	preload_tls = tls;

	tlsclnr_register(preload_reclaim, tls);
}
/*
 * This assumes that the caller has performed appropriate preallocation, and
 * that the caller has pinned this thread of control to the current CPU.
 */
static inline void *node_alloc(struct radix_tree_root *root)
{
	void *node = NULL;
	struct preload_tls *tls;

	radix_tree_preload();

	tls = preload_tls;
	if (skp_likely(tls->nr)) {
		RT_BUG_ON(list_empty(&tls->head));
		tls->nr -= 1;
		node = tls->head.next;
		list_del(node);
		memset(node, 0, sizeof(struct radix_tree_node));

		root->alloc_mm += sizeof(struct radix_tree_node);
	}

	return node;
}

static inline void node_free(struct radix_tree_root *root, void *node)
{
	struct preload_tls *tls;

	preload_init();

	tls = preload_tls;

	/*释放掉陈旧的缓存*/
	if (skp_unlikely(tls->nr == NODE_CACAH_MAX)) {
		RT_BUG_ON(list_empty(&tls->head));
		struct list_head *old = tls->head.prev;
		list_del(old);
		free(old);
		tls->nr -= 1;
	}

	tls->nr += 1;
	list_add(node, &tls->head);

	root->alloc_mm -= sizeof(struct radix_tree_node);
}

void radix_tree_preload(void)
{
	struct list_head *node;
	struct preload_tls *tls;

	preload_init();

	tls = preload_tls;
	while (tls->nr < NODE_CACAH_MAX/2) {

#ifdef UMALLOC_MANGLE
		node = umem_cache_alloc(node_allocator);
#else
		node = malloc(sizeof(struct radix_tree_node));
#endif
		if (WARN_ON(!node))
			break;
		tls->nr++;
		list_add_tail(node, &tls->head);
	}
}

void radix_tree_reclaim(void)
{
	__preload_reclaim(preload_tls);
}

static inline void tag_set(struct radix_tree_node *node, int tag, int offset)
{
	if (!test_bit(offset, &node->tags[tag][0]))
		__set_bit(offset, &node->tags[tag][0]);
}

static inline void tag_clear(struct radix_tree_node *node, int tag, int offset)
{
	__clear_bit(offset, &node->tags[tag][0]);
}

static inline int tag_get(struct radix_tree_node *node, int tag, int offset)
{
	return test_bit(offset, &node->tags[tag][0]);
}

/*
 *	Return the maximum key which can be store into a
 *	radix tree with height HEIGHT.
 */
static inline uint64_t rdtree_maxindex(uint32_t height)
{
	if (skp_unlikely(!radix_tree_up)) {
		big_lock();
		if (!radix_tree_up) {
			rdtree_init_maxindex();
			radix_tree_up = true;
		}
		big_unlock();
	}
	return height_to_maxindex[height];
}

/*
 *	Extend a radix tree so it can store key @index.
 * 扩展树，按 栈 的方式改变：
 * root --> [new node] <- 如果 tag 被设置 则设置
 *           | index = 0 扩展 0 位置上的节点
 *           v
 *          [new node] <- 如果 tag 被设置 则设置
 *           | index = 0 扩展 0 位置上的节点
 *           v
 *          [src node] <- 如果有 tag 设置 需要记录
 */
static int rdtree_extend(struct radix_tree_root *root, uint64_t index)
{
	uint32_t height;
	struct radix_tree_node *node;
	char tags[RADIX_TREE_TAGS];

	/* Figure out what the height should be.  */
	height = root->height + 1;
	while (index > rdtree_maxindex(height))
		height++;

	if (root->rnode == NULL) {
		root->height = height;
		goto out;
	}

	/*
	 * Prepare the tag status of the top-level node for propagation
	 * into the newly-pushed top-level node(s)
	 */
	for (int tag = 0; tag < RADIX_TREE_TAGS; tag++) {
		tags[tag] = 0;
		if (find_first_bit(root->rnode->tags[tag], RADIX_TREE_MAP_SIZE) <
				RADIX_TREE_MAP_SIZE)
			tags[tag] = 1;
	}

	do {
		if (!(node = node_alloc(root)))
			return -ENOMEM;

		/* Increase the height.  */
		node->slots[0] = root->rnode;

		/* Propagate the aggregated tag info into the new root */
		for (int tag = 0; tag < RADIX_TREE_TAGS; tag++) {
			if (tags[tag])
				tag_set(node, tag, 0);
		}

		node->count = 1;
		root->rnode = node;
		root->height++;
	} while (height > root->height);
out:
	return 0;
}

/*
 * 重要的技巧
 * 当 shift 不为0时 求倍数
 * 当 shift 为0时 求余数
 */
#define calc_offset(idx, shift)	\
	((uint32_t)(((idx) >> (shift)) & RADIX_TREE_MAP_MASK))
#define loc_slot(pslot, offset)	\
	((struct radix_tree_node **)(&READ_ONCE(*(pslot))->slots[(offset)]))

/**
 *	radix_tree_insert    -    insert into a radix tree
 *	@root:		radix tree root
 *	@index:		index key
 *	@item:		item to insert
 *
 *	Insert an item into the radix tree at position @index.
 */
int __radix_tree_insert(struct radix_tree_root *root, uint64_t index,
		void *item, struct radix_tree_node ***pslot)
{
	int error;
	uint32_t height, shift, offset = 0;
	struct radix_tree_node *parent = NULL, *tmp, **slot;

	if (skp_unlikely(!item))
		return -EINVAL;

	/* Make sure the tree is high enough.  */
	if ((!index && !root->rnode) ||
			index > rdtree_maxindex(root->height)) {
		error = rdtree_extend(root, index);
		if (skp_unlikely(error))
			return error;
	}

	slot = &root->rnode;
	height = root->height;
	shift = (height-1) * RADIX_TREE_MAP_SHIFT;

	while (height > 0) {
		if (READ_ONCE(*slot) == NULL) {
			/* Have to add a child node.  */
			if (!(tmp = node_alloc(root)))
				return -ENOMEM;
			//smp_wmb();
			WRITE_ONCE(*slot, tmp);
			//smp_mb();
			/*增加父节点的使用数量*/
			if (skp_likely(parent))
				parent->count++;
		}

		/* Go a level down
		 * 求取本层的 槽位 偏移
		 * 对于 某个高度的树 每一层 每个槽位表示的数字范围是递减，一个倍数
		 * 比如 3 层树
		 * 第1层：64 * 64 = 64^2 = 2 << 6
		 * 第2层：64 = 64^1 = 1 << 6
		 * 第3层：1 = 64^0 = 0 << 6 叶节点
		 * 恰好为 shift 的 运算 ：(height - 1) * map_shift
		 * 那么对于一个数，除以本层的倍数，就得到相应的范围下标了
		 * 也就是 index >> shift
		 */
		offset = calc_offset(index, shift);
		parent = *slot;
		slot = loc_slot(slot, offset);
		/*递减倍数*/
		shift -= RADIX_TREE_MAP_SHIFT;
		/*递减高度，直到叶节点*/
		height--;
	}

	if (*slot != NULL)
		return -EEXIST;

	root->nr_nodes++;
	if (skp_likely(parent)) {
		parent->count++;
		/*check tag*/
#ifdef DEBUG
		for (int tag = 0; tag < RADIX_TREE_TAGS; tag++) {
			BUG_ON(tag_get(parent, tag, offset));
		}
#endif
	}

	//smp_wmb();
	WRITE_ONCE(*slot, item);
	//smp_mb();

	if (pslot)
		*pslot = slot;
	return 0;
}

/**
 *	radix_tree_lookup    -    perform lookup operation on a radix tree
 *	@root:		radix tree root
 *	@index:		index key
 *
 *	Lookup the item at the position @index in the radix tree @root.
 */
void *__radix_tree_lookup(struct radix_tree_root *root, uint64_t index,
		struct radix_tree_node ***pslot)
{
	uint32_t height, shift, offset;
	struct radix_tree_node ** slot;

	height = root->height;
	if (index > rdtree_maxindex(height))
		return NULL;

	slot = &root->rnode;
	shift = (height-1) * RADIX_TREE_MAP_SHIFT;

	while (height > 0) {
		if (READ_ONCE(*slot) == NULL)
			return NULL;

		offset = calc_offset(index, shift);
		slot = loc_slot(slot, offset);
		shift -= RADIX_TREE_MAP_SHIFT;
		height--;
	}

	if (pslot)
		*pslot = slot;
	
	return READ_ONCE(*slot);
}

/**
 * 设置对应索引所有路径中的节点的标记
 *	radix_tree_tag_set - set a tag on a radix tree node
 *	@root:		radix tree root
 *	@index:		index key
 *	@tag: 		tag index
 *  @return 索引对应的值（页地址）
 *
 *	Set the search tag corresponging to @index in the radix tree.  From
 *	the root all the way down to the leaf node.
 *
 *	Returns the address of the tagged item.   Setting a tag on a not-present
 *	item is a bug.
 */
void *radix_tree_tag_set(struct radix_tree_root *root, uint64_t index, int tag)
{
	uint32_t height, shift, offset;
	struct radix_tree_node **slot;

	BUG_ON(tag >= RADIX_TREE_TAGS);

	height = root->height;
	if (index > rdtree_maxindex(height))
		return NULL;

	/*
	 * height:
	 * 1, 2, 3, 4, 5, 6
	 * shift :
	 * 0, 6, 12, 18, 24, 30, ...
	 */
	slot = &root->rnode;
	shift = (height - 1) * RADIX_TREE_MAP_SHIFT;

	while (height > 0) {
		offset = calc_offset(index, shift);
		tag_set(*slot, tag, offset);
		slot = loc_slot(slot, offset);
		BUG_ON(*slot == NULL);
		shift -= RADIX_TREE_MAP_SHIFT;
		height--;
	}

	return *slot;
}

static void *__fill_path(struct radix_tree_root *root, uint64_t index,
		struct radix_tree_path **pathpp)
{
	uint32_t height, shift, offset;
	struct radix_tree_path *pathp = *pathpp;

	height = root->height;
	if (index > rdtree_maxindex(height))
		return NULL;

	pathp->parent = NULL;
	pathp->slot = &root->rnode;
	shift = (height - 1) * RADIX_TREE_MAP_SHIFT;
	// 正向生成路径，每一个下标与所在层数一一对应
	while (height > 0) {
		if (*pathp[0].slot == NULL)
			return NULL;

		offset = calc_offset(index, shift);
		pathp[1].offset = offset;
		pathp[1].parent = *pathp[0].slot;
		pathp[1].slot = loc_slot(pathp[0].slot, offset);
		shift -= RADIX_TREE_MAP_SHIFT;
		height--;
		pathp++;
	}
	*pathpp = pathp;
	return *pathp[0].slot;
}

static void __clear_node_tag(struct radix_tree_path *pathp, int tag)
{
	/*反向遍历路径信息*/
	BUG_ON(!pathp[0].parent);
	do {
		/*清理当前层*/
		if (!tag_get(pathp[0].parent, tag, pathp[0].offset))
			goto out;
		tag_clear(pathp[0].parent, tag, pathp[0].offset);
		/*其他槽位有设置*/
		if (find_first_bit(pathp[0].parent->tags[tag], RADIX_TREE_MAP_SIZE) <
				RADIX_TREE_MAP_SIZE)
			break;
		pathp--;
	} while (pathp[0].parent);
out:
	return;
}

/**
 *	radix_tree_tag_clear - clear a tag on a radix tree node
 *	@root:		radix tree root
 *	@index:		index key
 *	@tag: 		tag index
 *  @return 索引对应的值（页地址）
 *
 *	Clear the search tag corresponging to @index in the radix tree.  If
 *	this causes the leaf node to have no tags set then clear the tag in the
 *	next-to-leaf node, etc.
 *
 *	Returns the address of the tagged item on success, else NULL.  ie:
 *	has the same return value and semantics as radix_tree_lookup().
 */
void *radix_tree_tag_clear(struct radix_tree_root *root, uint64_t index, int tag)
{
	void *value = NULL;
	struct radix_tree_path path[RADIX_TREE_MAX_PATH], *pathp = path;

	BUG_ON(tag >= RADIX_TREE_TAGS);

	value = __fill_path(root, index, &pathp);
	if (!value)
		goto out;
	__clear_node_tag(pathp, tag);
out:
	return value;
}

/**
 *	radix_tree_tag_get - get a tag on a radix tree node
 *	@root:		radix tree root
 *	@index:		index key
 *	@tag: 		tag index
 *
 *	Return the search tag corresponging to @index in the radix tree.
 *
 *	Returns zero if the tag is unset, or if there is no corresponding item
 *	in the tree.
 */
int radix_tree_tag_get(struct radix_tree_root *root, uint64_t index, int tag)
{
	int rc = 0, saw_unset_tag = 0;
	uint32_t height, shift, offset;
	struct radix_tree_node **slot;

	BUG_ON(tag >= RADIX_TREE_TAGS);

	height = root->height;
	if (index > rdtree_maxindex(height))
		return 0;

	shift = (height - 1) * RADIX_TREE_MAP_SHIFT;
	slot = &root->rnode;

	while(*slot) {
		offset = calc_offset(index, shift);
		/*
		 * This is just a debug check.  Later, we can bale as soon as
		 * we see an unset tag.
		 * 如果中间层没有标记则设置
		 */
		if (!tag_get(*slot, tag, offset))
			saw_unset_tag = 1;
		if (height == 1) {
			/*最后一层便是真正对应的tag*/
			rc = tag_get(*slot, tag, offset);
			break;
		}
		slot = loc_slot(slot, offset);
		shift -= RADIX_TREE_MAP_SHIFT;
		height--;
	}
	BUG_ON(rc && saw_unset_tag);
	return rc;
}

static uint32_t __lookup(const struct radix_tree_root *root, void **results,
		uint64_t index, uint32_t max_items, uint64_t *next_index)
{
	struct radix_tree_node *slot;
	uint32_t offset, shift, nr_found = 0, height = root->height;

	shift = (height-1) * RADIX_TREE_MAP_SHIFT;
	slot = root->rnode;

	while (height > 0) {
		offset = calc_offset(index, shift);
		for ( ; offset < RADIX_TREE_MAP_SIZE; offset++) {
			if (slot->slots[offset] != NULL)
				break;
			/*该层槽位的移动必须是该层槽位对应倍数对齐的，可以类比页表遍历*/
			index &= ~((1ULL << shift) - 1);
			/*移动到下一个槽位对应的起始索引*/
			index += 1ULL << shift;
			if (index == 0)
				goto out;	/* 64-bit wraparound */
		}
		if (offset == RADIX_TREE_MAP_SIZE)
			goto out;
		if (height == 1) {	/* Bottom level: grab some items */
			uint32_t j = calc_offset(index, 0);
			for ( ; j < RADIX_TREE_MAP_SIZE; j++) {
				index++;
				if (slot->slots[j]) {
					results[nr_found++] = slot->slots[j];
					if (nr_found == max_items)
						goto out;
				}
			}
		}
		slot = slot->slots[offset];

		shift -= RADIX_TREE_MAP_SHIFT;
		height--;
	}
out:
	*next_index = index;
	return nr_found;
}

/**
 *	radix_tree_gang_lookup - perform multiple lookup on a radix tree
 *	@root:		radix tree root
 *	@results:	where the results of the lookup are placed
 *	@first_index:	start the lookup from this key
 *	@max_items:	place up to this many items at *results
 *
 *	Performs an index-ascending scan of the tree for present items.  Places
 *	them at *@results and returns the number of items which were placed at
 *	*@results.
 *
 *	The implementation is naive.
 */
uint32_t __radix_tree_gang_lookup(const struct radix_tree_root *root,
		uint64_t first_index, void **results, uint32_t max_items, uint64_t *pnext_index)
{
	uint32_t ret = 0, nr_found = 0;
	uint64_t next_index = 0, cur_index = first_index;
	const uint64_t max_index = rdtree_maxindex(root->height);

	while (ret < max_items) {

		if (cur_index > max_index)
			break;
		nr_found = __lookup(root, results + ret, cur_index,
						max_items - ret, &next_index);
		ret += nr_found;
		if (next_index == 0)
			break;
		cur_index = next_index;
	}
	if (pnext_index)
		*pnext_index = next_index;
	return ret;
}

/**
 * 以参数index为起始索引，遍历后续的索引中标记位有效的索引，并提取对应的外部对象到result数组中，
 * 最多提取max_items个，next_index是下一次可以接着本次遍历的索引
 * FIXME: the two tag_get()s here should use find_next_bit() instead of
 * open-coding the search.
 */
static uint32_t __lookup_tag(const struct radix_tree_root *root,
		uint64_t index, void **results, uint32_t max_items, uint64_t *next_index, int tag)
{
	struct radix_tree_node *slot;
	uint32_t shift, nr_found = 0, height = root->height;

	shift = (height - 1) * RADIX_TREE_MAP_SHIFT;
	slot = root->rnode;

	while (height > 0) {
		// 获取本层的索引（2进制6位）
		uint64_t i = calc_offset(index, shift), j = i;
		// 从起始所有遍历本层的后面的槽位对应的标记，找出有标记的槽位，然后调整index为相应的值
		i = find_next_bit(&slot->tags[tag][0], RADIX_TREE_MAP_SIZE, j);
		index += (i - j) << shift;
		if (i >= RADIX_TREE_MAP_SIZE) {
			index &= ~((1ULL << shift) - 1);// 屏蔽掉低位
			goto out;
		} else if (i != j) {
			index &= ~((1ULL << shift) - 1);// 屏蔽掉低位
		}

		//到达叶子节点
		if (height == 1) {	/* Bottom level: grab some items */
			uint64_t i = calc_offset(index, 0), j = i;
			for_each_set_bit_from(i, slot->tags[tag], RADIX_TREE_MAP_SIZE) {
				BUG_ON(slot->slots[i] == NULL);
				results[nr_found++] = slot->slots[i];
				if (nr_found == max_items) {
					index += i - j + 1;
					goto out;
				}
			}
			index += RADIX_TREE_MAP_SIZE;
			index &= ~ RADIX_TREE_MAP_MASK;
		}
		shift -= RADIX_TREE_MAP_SHIFT;
		slot = slot->slots[i];
		height--;
	}
out:
	*next_index = index;
	return nr_found;
}

/**
 *	radix_tree_gang_lookup_tag - perform multiple lookup on a radix tree
 *	                             based on a tag
 *	@root:		radix tree root
 *	@results:	where the results of the lookup are placed
 *	@first_index:	start the lookup from this key
 *	@max_items:	place up to this many items at *results
 *	@tag:		the tag index
 *
 *	Performs an index-ascending scan of the tree for present items which
 *	have the tag indexed by @tag set.  Places the items at *@results and
 *	returns the number of items which were placed at *@results.
 */
uint32_t __radix_tree_gang_lookup_tag(const struct radix_tree_root *root,
		uint64_t first_index, void **results, uint32_t max_items,
		int tag, uint64_t *pnext_index)
{
	uint32_t ret = 0, nr_found;
	uint64_t next_index = 0, cur_index = first_index;
	const uint64_t max_index = rdtree_maxindex(root->height);

	BUG_ON(tag >= RADIX_TREE_TAGS);

	while (ret < max_items) {
		if (cur_index > max_index)
			break;
		nr_found = __lookup_tag(root, cur_index, results + ret,
			max_items - ret, &next_index, tag);
		ret += nr_found;
		if (next_index == 0)
			break;
		cur_index = next_index;
	}
	if (pnext_index)
		*pnext_index = next_index;
	return ret;
}

/**
 *	radix_tree_delete    -    delete an item from a radix tree
 *	@root:		radix tree root
 *	@index:		index key
 *
 *	Remove the item at @index from the radix tree rooted at @root.
 *
 *	Returns the address of the deleted item, or NULL if it was not present.
 */
void *radix_tree_delete(struct radix_tree_root *root, uint64_t index)
{
	void *ret = NULL;
	struct radix_tree_path path[RADIX_TREE_MAX_PATH], *pathp = path;

	ret = __fill_path(root, index, &pathp);
	if (!ret)
		goto out;

	/*
	 * Clear all tags associated with the just-deleted item
	 */
	for (int tag = 0; tag < RADIX_TREE_TAGS; tag++)
		__clear_node_tag(pathp, tag);

	root->nr_nodes--;
	/*移除被删除的值*/
	*pathp[0].slot = NULL;
	/*反向清除路径上没有使用的节点*/
	while (pathp[0].parent && --pathp[0].parent->count == 0) {
		pathp--;
		BUG_ON(*pathp[0].slot == NULL);
		*pathp[0].slot = NULL;
		node_free(root, pathp[1].parent);
	}
	if (root->rnode == NULL)
		root->height = 0;
out:
	return ret;
}

/**
 *	radix_tree_tagged - test whether any items in the tree are tagged
 *	@root:		radix tree root
 *	@tag:		tag to test
 */
bool radix_tree_tagged(struct radix_tree_root *root, int tag)
{
	BUG_ON(tag >= RADIX_TREE_TAGS);
	if (!root->rnode)
		return false;
	return !!(find_first_bit(root->rnode->tags[tag], RADIX_TREE_MAP_SIZE) <
		RADIX_TREE_MAP_SIZE);
}

void *radix_tree_iter_next(const struct radix_tree_root *root,
		struct radix_tree_iter *iter)
{
	void *ptr;
	uint32_t nr;
	uint64_t index = iter->next_index;
	if (!index && iter->index)
		return NULL;
	nr = __radix_tree_gang_lookup(root, index, &ptr, 1, &iter->next_index);
	if (!nr) {
		iter->index = U64_MAX;
		return NULL;
	}
	iter->index = iter->next_index - 1;
	return ptr;
}

void *radix_tree_iter_next_tag(const struct radix_tree_root *root,
		struct radix_tree_iter *iter, int tag)
{
	void *ptr;
	uint32_t nr;
	uint64_t index = iter->next_index;

	BUG_ON(tag >= RADIX_TREE_TAGS);

	if (!index && iter->index)
		return NULL;
	nr = __radix_tree_gang_lookup_tag(root, index, &ptr, 1, tag, &iter->next_index);
	if (!nr) {
		iter->index = U64_MAX;
		return NULL;
	}
	iter->index = iter->next_index - 1;
	return ptr;
}

void radix_tree_release(struct radix_tree_root *root,
		radix_tree_fn free_fn, void *user)
{
	void *ptr, *tmp;

	radix_tree_for_each(ptr, root, 0) {
		if (free_fn)
			free_fn(ptr, radix_tree_iter_index(), user);
		tmp = radix_tree_delete(root, radix_tree_iter_index());
		BUG_ON(tmp != ptr);
	}

	BUG_ON(root->rnode);
}
