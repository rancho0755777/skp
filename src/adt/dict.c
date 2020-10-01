#include <skp/adt/dict.h>
#include <skp/mm/slab.h>

////////////////////////////////////////////////////////////////////////////////

struct dict_bucket {
	struct dict_node *head;
#ifdef DICT_DEBUG
	uint32_t nr_nodes; /*节点数*/
#endif
};

#define __prefetch_node(x) prefetch((x))
#define DICT_INIT_SIZE (1U << 7)
#define DICT_EXPAND_RATIO (8)
#define DICT_REDUCE_RATIO (2)

////////////////////////////////////////////////////////////////////////////////
static __always_inline void __erase_node_after(struct dict_table *, uint32_t);
static __always_inline void __insert_node_before(struct dict_table *, uint32_t);
static inline struct dict_bucket *__get_bucket(
	const struct dict_table *, uint32_t, uint32_t *);

////////////////////////////////////////////////////////////////////////////////
// 重写以下函数可以实现以不同基础数据结构作为哈希桶
////////////////////////////////////////////////////////////////////////////////
#ifdef DICT_DEBUG

static inline void __bucket_update(struct dict_bucket *bucket, bool del)
{
	if (del)
		--bucket->nr_nodes;
	else
		++bucket->nr_nodes;
}
static inline void __bucket_init(struct dict_bucket *bucket) /*初始化桶*/
{
	bucket->head = NULL;
	bucket->nr_nodes = 0;
}

static inline void __dict_acct_expand(struct dict *dict)
{
	dict->stats.nr_expands++;
}

static inline void __dict_acct_reduce(struct dict *dict)
{
	dict->stats.nr_reduces++;
}

#define DICT_BUG_ON(x) BUG_ON(x)
#else
# define __bucket_update(bucket, del)
# define __dict_acct_expand(dict)
# define __dict_acct_reduce(dict)
static inline void __bucket_init(struct dict_bucket *bucket) /*初始化桶*/
{
	bucket->head = NULL;
}
#define DICT_BUG_ON(x)
#endif

/*实际实现哈希节点的数据结构*/

typedef struct dict_node * dictnode_t;

#define __bucket_empty(b)	(skp_likely((b)) && (b)->head == NULL)
#define __dnode_linked(n)	(skp_likely((n)) && (n)->next != DICT_NODE_POSITION)

static inline dictnode_t __erase_first_node(struct dict_bucket *bucket)
{
	dictnode_t node = bucket->head;
	if (node) {
		bucket->head = node->next;
		node->next = DICT_NODE_POSITION;
	}
	return node;
}

static inline void __insert_node(struct dict_bucket *bucket, dictnode_t node,
		dictnode_t *link)
{
	node->next = *link;
	*link = node;
}

static inline void __erase_node(struct dict_bucket *bucket, dictnode_t node,
		dictnode_t *link)
{
	*link = node->next;
	node->next = DICT_NODE_POSITION;
}

static inline uint32_t __get_nodes(struct dict_bucket *bucket)
{
#ifndef XPRT_DEBUG
	uint32_t chainlen = 0;
	for (dictnode_t p = bucket->head;
			p && p != DICT_NODE_POSITION ; p = p->next) {
		chainlen++;
	}
	return chainlen;
#else
	return READ_ONCE(bucket->nr_nodes);
#endif
}

static struct dict_node *
__bucket_lookup_key(struct dict *dict, struct dict_bucket *bucket,
		const void *key, uint32_t hvalue, dictnode_t **linkptr)
{
	dictnode_t *link;
	const struct dict_ops *ops = dict->ops;

	link = &bucket->head;
	while (*link) {
		__prefetch_node(*link);
		if (!ops->compare_key(*link, key, hvalue))
			break;
		link = &(*link)->next;
	}
	*linkptr = link;
	return *link;
}

static struct dict_node *
__bucket_lookup_node(struct dict *dict, struct dict_bucket *bucket,
		dictnode_t src, dictnode_t **linkptr)
{
	dictnode_t *link;
	const struct dict_ops *ops = dict->ops;

	link = &bucket->head;
	while (*link) {
		__prefetch_node(*link);
		if (!ops->compare_node(*link, src))
			break;
		link = &(*link)->next;
	}
	*linkptr = link;
	return *link;
}

static __always_inline void __bucket_rehash(struct dict *dict,
	struct dict_bucket *bucket, uint32_t bit)
{
	uint32_t idx;
	dictnode_t *link;
	struct dict_node *node;
	struct dict_bucket *desc;

	while (1) {
		/*remove from rbtree*/
		node = __erase_first_node(bucket);
		if (skp_unlikely(!node))
			break;
		DICT_BUG_ON((node->whichb?
			&dict->tables[1]:&dict->tables[0]) != dict->active);
		DICT_BUG_ON(node->bindex != bit);

		__erase_node_after(dict->active, bit);

		/*prepare insert*/
		desc = __get_bucket(dict->rehash, node->hvalue, &idx);

#ifdef XPRT_DEBUG
{
		struct dict_node *tmp;
		tmp = __bucket_lookup_node(dict, desc, node, &link);
		BUG_ON(tmp);
}
#else
		link = &desc->head;
#endif

		/*update mapping*/
		__insert_node_before(dict->rehash, idx);
		__insert_node(desc, node, link);

		node->bindex = idx;
		node->whichb = dict->rehash == &dict->tables[0] ? 0 : 1;
	}
	DICT_BUG_ON(!__bucket_empty(bucket));
	DICT_BUG_ON(bucket->nr_nodes);
}
////////////////////////////////////////////////////////////////////////////////
static inline void __erase_node_after(struct dict_table *dtable, uint32_t idx)
{
	struct dict_bucket *bucket = &dtable->bucket[idx];
	dtable->nr_nodes--;
	if (skp_unlikely(__bucket_empty(bucket)))
		__clear_bit(idx, dtable->mapping);
	__bucket_update(bucket, true);
}

static inline void __insert_node_before(struct dict_table *dtable, uint32_t idx)
{
	struct dict_bucket *bucket = &dtable->bucket[idx];
	if (skp_unlikely(__bucket_empty(bucket)))
		__set_bit(idx, dtable->mapping);
	dtable->nr_nodes++;
	__bucket_update(bucket, false);
}
////////////////////////////////////////////////////////////////////////////////
/*return false means rehash incomplete*/
static bool __dict_rehash(struct dict *dict, int64_t step);

static inline bool ___dict_is_rehashing(const struct dict *dict)
{
	return !!(READ_ONCE(dict->rehashidx) != -1);
}

static inline bool __dict_is_rehashing(const struct dict *dict)
{
	return skp_unlikely(!!(READ_ONCE(dict->rehashidx) != -1));
}

static __always_inline bool __dict_check_rehashing(struct dict *dict)
{
	if (__dict_is_rehashing(dict)) {
#ifndef DEBUG
		uint32_t curs = READ_ONCE(dict->active->nr_nodes) / dict->active->size;
		return __dict_rehash(dict, clamp_t(uint32_t, curs, 2, 8));
#else
		return __dict_rehash(dict, 1);
#endif
	}
	return false;
}

static __always_inline struct dict_bucket *__get_bucket(
	const struct dict_table *dtable, uint32_t hvalue, uint32_t *idx)
{
	struct dict_bucket *hb;
	DICT_BUG_ON(!is_power_of_2(dtable->size));
	*idx = hvalue & (dtable->size - 1);
	hb = &dtable->bucket[*idx];
	prefetch(hb);
	return hb;
}

static int dict_table_init(struct dict_table *dtable, uint32_t size, struct dict *dict)
{
	unsigned long *mapping;
	struct dict_bucket *bucket;

	DICT_BUG_ON(READ_ONCE(dtable->nr_nodes));
	DICT_BUG_ON(size && !is_power_of_2(size));

	dtable->size = size;
	dtable->nr_nodes = 0;
	dtable->bucket = NULL;
	dtable->mapping = NULL;

	/*size == 0 是反初始化*/
	if (!size) {
		if (dtable == dict->rehash && ___dict_is_rehashing(dict))
			dict->rehashidx = -1;
		return 0;
	}

	/*前半段放位图信息*/
	mapping = malloc(BITMAP_SIZE(size));
	if (skp_unlikely(!mapping))	{
		log_error("allocate memory for mapping failed");
		return -ENOMEM;
	}
	bucket = malloc(size * sizeof(*bucket));
	if (skp_unlikely(!bucket))	{
		free(mapping);
		log_error("allocate memory for bucket failed");
		return -ENOMEM;
	}
	/*initial table*/
	bitmap_zero(mapping, size);
	for (uint32_t i = 0; i < size; i++) {
		__bucket_init(&bucket[i]);
	}

	dtable->mapping = mapping;
	dtable->bucket = bucket;

	/*构造中间表*/
	if (dtable == dict->rehash)
		dict->rehashidx = 0;

	return 0;
}

static inline void table_release(struct dict_table *dtable, struct dict *dict)
{
	DICT_BUG_ON(READ_ONCE(dtable->nr_nodes));
	free(dtable->mapping);
	free(dtable->bucket);
	dict_table_init(dtable, 0, dict);
}

bool dict_check_rehashing(struct dict *dict)
{
	return __dict_check_rehashing(dict);
}

static bool __dict_resize_if_needed(struct dict *dict, bool isexpand)
{
	int rc = 0;
	/*todo : 使用小数计算当前平均长度*/
	double cur;
	uint32_t old, size;
	struct dict_table *active;

	/*此段判断未加锁，所以可能不能及时进行rehash，这是允许的*/
	active = dict->active;
	cur = READ_ONCE(active->nr_nodes);
	if (skp_unlikely(!cur) || __dict_is_rehashing(dict))
		return false;
	old = active->size;
	if (isexpand && skp_likely(dict->expand_ratio != U16_MAX) &&
			cur >= old && skp_unlikely(cur / old > dict->expand_ratio)) {
		size = max(old, old << 1);
		if (skp_unlikely(size == old || size == 0))
			return false;
	} else if (!isexpand && cur < old && skp_likely(dict->expand_ratio != 1) &&
			skp_unlikely(cur / old < dict->reduce_ratio)) {
		size = max(dict->init_size, old >> 1);
		if (skp_unlikely(size == old || size == 0))
			return false;
	} else {
		return false;
	}

	rc = dict_table_init(dict->rehash, size, dict);
	if (skp_unlikely(rc)) {
		log_error("out of memery : %u", size);
		return false;
	}
#ifdef DEBUG
	log_debug("\n\tREHASH : ratio %lf (%u), %s [%u] -> [%u]\n"
		"\tdict current nodes %u\n"
		, cur / old, dict->expand_ratio
		, isexpand ? "expand" : "reduce", old, size
		, dict_nodes(dict));
#endif
	if (isexpand) {
		__dict_acct_expand(dict);
	} else {
		__dict_acct_reduce(dict);
	}
	return true;
}
////////////////////////////////////////////////////////////////////////////////

/*初始化*/
int dict_init(struct dict *dict, const struct dict_attr *attr)
{
	int rc;

	BUG_ON(!dict);
	BUG_ON(!attr);
	BUG_ON(!attr->ops);
	BUG_ON(!attr->ops->calc_hash);
	BUG_ON(!attr->ops->compare_key);
	BUG_ON(!attr->ops->compare_node);

#ifdef DICT_DEBUG
	memset(&dict->stats, 0, sizeof(dict->stats));
#endif

	dict->rehashidx = -1;
	dict->tables[0].nr_nodes = 0;
	dict->tables[1].nr_nodes = 0;
	dict->active = &dict->tables[0];
	dict->rehash = &dict->tables[1];
	dict->ops = attr->ops;
	dict->expand_ratio = attr->expand_ratio?:DICT_EXPAND_RATIO;
	dict->reduce_ratio = attr->reduce_ratio?:DICT_REDUCE_RATIO;
	dict->init_size = attr->init_size?:DICT_INIT_SIZE;
	dict->init_size = (uint32_t)roundup_pow_of_two(dict->init_size);
	BUG_ON(dict->expand_ratio <= dict->reduce_ratio);

	rc = dict_table_init(dict->active, dict->init_size, dict);
	if (skp_unlikely(rc))
		return rc;

	dict_table_init(dict->rehash, 0, dict);

	return 0;
}

void dict_release(struct dict *dict, dict_fn free_fn, void *user_data)
{
	if (free_fn) {
		struct dict_node *dnode;
		dict_for_each(dnode, dict) {
			struct dict_table *tab = &dict->tables[dnode->whichb];
			tab->nr_nodes--;
			free_fn(dnode, user_data);
		}
	}

	table_release(dict->active, dict);
	if (__dict_is_rehashing(dict))
		table_release(dict->rehash, dict);

	dict->rehashidx = -1;
}

/*总是先查找active*/
#define __declare_lookup()								\
	uint32_t _hashval, _bucketidx;						\
	struct dict_table *_hashtab;						\
	struct dict_bucket *_bucket;						\
	dictnode_t *_link

#define __start_lookup(dict, key)						\
({ 														\
	dictnode_t __node;		 							\
	_hashtab = (dict)->active; 							\
	_hashval = (dict)->ops->calc_hash((key));			\
	_bucket = __get_bucket(								\
		_hashtab, _hashval, &_bucketidx);				\
	__node = __bucket_lookup_key((dict), _bucket, 		\
		(key), _hashval, &_link);						\
	if (!__node && __dict_is_rehashing((dict))) { 		\
		_hashtab = (dict)->rehash;						\
		_bucket = __get_bucket(							\
			_hashtab, _hashval, &_bucketidx);			\
		__node = __bucket_lookup_key((dict), _bucket, 	\
			(key), _hashval, &_link); 					\
	}													\
	__node;												\
})

/*查找与更新*/
struct dict_node *
dict_insert(struct dict *dict, const void *key, struct dict_node *node)
{
	struct dict_node *old;
	__declare_lookup();

	__dict_check_rehashing(dict);
	__dict_resize_if_needed(dict, true);

	old = __start_lookup(dict, key);
	if (old)
		return old;

	/*insert*/
	node->hvalue = _hashval;
	node->whichb = _hashtab == &dict->tables[0] ? 0 : 1;
	node->bindex = _bucketidx;

	/*update mapping*/
	__insert_node_before(_hashtab, _bucketidx);
	__insert_node(_bucket, node, _link);

	return 0;
}

/*查找*/
struct dict_node *dict_lookup(struct dict *dict, const void *key)
{
	__declare_lookup();
	if (!dict_nodes(dict))
		return NULL;
	return __start_lookup(dict, key);
}

/*查找移除*/
struct dict_node *dict_lookup_remove(struct dict *dict, const void *key)
{
	struct dict_node *node;

	__declare_lookup();

	/*check rehash*/
	__dict_check_rehashing(dict);

	if (!dict_nodes(dict))
		return NULL;

	node = __start_lookup(dict, key);
	if (!node)
		return NULL;
	/*remove*/
	__erase_node(_bucket, node, _link);
	__erase_node_after(_hashtab, _bucketidx);
	/*must be here to check whether it is in rbtree*/
	__dict_resize_if_needed(dict, false);
	dict_node_init(node);
	return node;
}

/*直接移除*/
int dict_direct_remove(struct dict *dict, struct dict_node *node)
{
	uint32_t idx;
	struct dict_table *_hashtab;
	struct dict_bucket *_bucket;
	dictnode_t tmp, *link = NULL;

	if (!__dnode_linked(node))
		return -EINVAL;

	/*must hold lock to check*/
	_hashtab = &dict->tables[node->whichb];
	_bucket = __get_bucket(_hashtab, node->hvalue, &idx);
	BUG_ON(idx != node->bindex);

	tmp = __bucket_lookup_node(dict, _bucket, node, &link);
	BUG_ON(tmp != node);

	/*remove*/
	__erase_node(_bucket, node, link);
	__erase_node_after(_hashtab, idx);
	__dict_resize_if_needed(dict, false);
	dict_node_init(node);
	/*delete success*/

	return 0;
}

static bool __dict_rehash(struct dict *dict, int64_t step)
{
	bool rc = true;
	struct dict_bucket *bucket;
	unsigned long index;

	if (!___dict_is_rehashing(dict))
		return false;

	while (step--) {
		if (!READ_ONCE(dict->active->nr_nodes)) {
			/*rehash has been completed*/
			swap(dict->active, dict->rehash);
			static_mb();
			table_release(dict->rehash, dict);
			log_debug("\n\tREHASH : completed [%u]\n"
				"\tdict current nodes %u\n"
				, dict->active->size
				, dict_nodes(dict));
			rc = false;
			break;
		}

		/*found non-zero slot by mapping*/
		index = find_next_bit(dict->active->mapping,
			dict->active->size, (unsigned long)dict->rehashidx);
		DICT_BUG_ON(index >= dict->active->size);
		dict->rehashidx = index + 1;

		bucket = &dict->active->bucket[index];
		/*move to other table*/
		DICT_BUG_ON(!dict->rehash->size);
		__bucket_rehash(dict, bucket, (uint32_t)index);
	}
	return rc;
}
/////////////////////////////////////////////////////////////////////////////
#ifdef DICT_DEBUG
#define LOG(...) fprintf(stderr, ##__VA_ARGS__)
#define DICT_STATS_VECTLEN 64U

static void __hashtab_stats_print(struct dict *dict,
	struct dict_table *dtable, bool onlystat)
{
	uint32_t cur, slots = 0, chainlen, maxchainlen = 0;
	uint32_t totchainlen = 0;
	uint32_t clvector[DICT_STATS_VECTLEN];

	cur = READ_ONCE(dtable->nr_nodes);

	if (!cur) {
		if (!onlystat) {
			LOG("Hash table stats:\n");
			LOG(" table size: %u\n", dtable->size);
			LOG(" number of elements: ZERO\n");
		}
		goto stat;
	}

	memset(clvector, 0, sizeof(clvector));

	/*遍历每个槽位，统计各种槽位的长度的槽位数量*/
	for (uint32_t i = 0; i < dtable->size; i++) {
		if (__bucket_empty(&dtable->bucket[i])) {
			clvector[0]++;
			continue;
		}
		slots++;
		chainlen = __get_nodes(&dtable->bucket[i]);
		clvector[(chainlen < DICT_STATS_VECTLEN) ?
			chainlen : (DICT_STATS_VECTLEN - 1)]++;
		maxchainlen = max(maxchainlen, chainlen);
		totchainlen += chainlen;
	}

stat:
	if (dtable == dict->active) {
		dict->stats.active_max_nodes = maxchainlen;
		dict->stats.active_avg_nodes = slots ? totchainlen / slots : 0;
		dict->stats.alloc_mm = dtable->size * sizeof(struct dict_bucket) +
			BITMAP_SIZE(dtable->size) + cur * sizeof(struct dict_node);
	} else {
		dict->stats.rehash_max_nodes = maxchainlen;
		dict->stats.rehash_avg_nodes = slots ? totchainlen / slots : 0;
		dict->stats.alloc_mm += dtable->size * sizeof(struct dict_bucket) +
			BITMAP_SIZE(dtable->size) + cur * sizeof(struct dict_node);

	}

	if (onlystat || !cur) {
		return;
	}

	LOG("Hash table stats:\n");
	LOG("   table size: %u/(%luKB)\n", dtable->size,
		(dtable->size * sizeof(struct dict_bucket) +
			BITMAP_SIZE(dtable->size)) >> 10);
	LOG(" number of elements: %u/(%luKB)\n", cur,
		(cur * sizeof(struct dict_node) >> 10));
	LOG("   non-empty slots: %u\n", slots);//不为零的不同槽位数量
	LOG("   max chain length: %u\n", maxchainlen);
	LOG("   avg chain length (counted): %u\n", totchainlen / slots);//越少，说明散列冲突越小
	LOG("   avg chain length (computed): %u\n", cur / slots);//越少，说明散列冲突越小
	LOG("   chain length distribution:\n");
	LOG("      slot - length : amount\n");
	/*长度最短（大于0）的槽位越多，说明性能越好，长度为0的越少，说明散列冲突越小*/
	for (uint32_t i = 0; i < DICT_STATS_VECTLEN; i++) {
		if (clvector[i] == 0) continue;
		LOG("      %s%u: %u (%.02f%%)\n",
			(i == DICT_STATS_VECTLEN - 1) ? ">= " : "", i, clvector[i],
			((float)clvector[i] / dtable->size) * 100);
	}
}

static void __dict_stats_print(struct dict *dict, bool onlystat)
{
	if (!onlystat) {
		LOG("=========================================================\n");
		LOG("Dict attr:\n");
		LOG("   initial size : %u\n", dict->init_size);
		LOG("   expand ratio : %u\n", dict->expand_ratio);
		LOG("   reduce ratio : %u\n", dict->reduce_ratio);
		LOG("Dict stats:\n");
		LOG("   expand times : %u\n", dict->stats.nr_expands);
		LOG("   reduce times : %u\n", dict->stats.nr_reduces);
	}

	dict->stats.avg_nodes = 0;
	dict->stats.max_nodes = 0;
	dict->stats.active_max_nodes = 0;
	dict->stats.rehash_max_nodes = 0;

/*一定要这样的顺序 : 先统计active*/
	__hashtab_stats_print(dict, dict->active, onlystat);

	if (__dict_is_rehashing(dict)) {
		if (!onlystat)
			LOG("-- Rehashing:\n");
		__hashtab_stats_print(dict, dict->rehash, onlystat);
		dict->stats.avg_nodes = dict->stats.rehash_avg_nodes ?
			(dict->stats.active_avg_nodes + dict->stats.rehash_avg_nodes) / 2 :
				dict->stats.active_avg_nodes;
	} else {
		dict->stats.avg_nodes = dict->stats.active_avg_nodes;
	}
	dict->stats.max_nodes = max(dict->stats.active_max_nodes,
		dict->stats.rehash_max_nodes);

	if (!onlystat)
		LOG("=========================================================\n");
}
#else
#define __dict_stats_print(d, o)
#endif

void dict_stats_print(struct dict *dict)
{
	__dict_stats_print(dict, false);
}

void dict_stats_colloect(struct dict *dict)
{
	__dict_stats_print(dict, true);
}

struct dict_iter *dict_iter_init(struct dict_iter *iter, struct dict *dict)
{
	unsigned long bit;
	iter->dict = dict;
	iter->next = NULL;
	if (dict_nodes(dict)) {
		bit = find_next_bit(dict->active->mapping, dict->active->size, 0);
		iter->next = dict->active->bucket[bit].head;
		BUG_ON(!iter->next);
	}
	return iter;
}

void dict_iter_release(struct dict_iter *iter)
{
	iter->dict = NULL;
	iter->next = NULL;
}

struct dict_node *dict_iter_next(struct dict_iter *iter)
{
	long bit;
	struct dict *dict;
	struct dict_table *tab;
	struct dict_node * next = iter->next;
	if (skp_unlikely(!next))
		return NULL;
	iter->next = next->next;
	if (skp_likely(iter->next))
		return next;
	dict = iter->dict;
	tab = &dict->tables[next->whichb];
	bit = next->bindex;
	if (skp_unlikely(bit >= tab->size)) {
try:
		if (tab == dict->rehash || !___dict_is_rehashing(dict))
			return next;
		bit = -1;
		tab = dict->rehash;
		log_debug("prepare iterate rehash table ...");
	}
	bit = find_next_bit(tab->mapping, tab->size, bit + 1);
	if (skp_unlikely(bit >= tab->size))
		goto try;
	iter->next = tab->bucket[bit].head;

	return next;
}
