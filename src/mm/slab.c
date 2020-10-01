//
//  slab.c
//
//  Created by 周凯 on 2019/3/4.
//  Copyright © 2019 zhoukai. All rights reserved.
//

#include <skp/mm/pgalloc.h>
#include <skp/utils/mutex.h>
#include <skp/utils/rwsem.h>
#include <skp/process/thread.h>
#include <skp/adt/rbtree.h>
#include <skp/algorithm/list_sort.h>

#include <skp/mm/slab.h>

//#define SLAB_DEBUG
//#define SLAB_LEAK_CHECK
//#define CONFIG_UMALLOC_HUGE

#define SLAB_MIN_ORDER 0

/*可以分配最大的对象大小*/
#ifdef CONFIG_UMALLOC_HUGE
/*可以分配超过虚拟页大小的内存*/
# define SLAB_MIN_OBJECTS (1U << 16)
# define SLAB_MAX_ORDER (MAX_ORDER - 1)
# define UMALLOC_SHIFT_HIGH (MAX_ORDER/2 + VPAGE_SHIFT - 1)
#else
# define SLAB_MIN_OBJECTS (1U << 12)
# define SLAB_MAX_ORDER (MAX_ORDER/3)
# define UMALLOC_SHIFT_HIGH (VPAGE_SHIFT - 1)
#endif

#define UMALLOC_SHIFT_LOW	(ilog2(UMALLOC_MIN_SIZE))
#define UMALLOC_MAX_SIZE	(1UL << UMALLOC_SHIFT_HIGH)
#define UMALLOC_MIN_SIZE	(8)

#define UMALLOC_MIN_PARTIAL (2)
#define UMALLOC_MAX_PARTIAL (3)

#define FROZEN (1 << PG_active)

#define UMALLOC_MINALIGN __alignof__(unsigned long)
#define SLAB_MINALIGN __alignof__(unsigned long)

/*tls分配器的索引*/
#define TLS_FREELIST_CAP 128
#define TLS_ALLOCATER_OBJSIZE (sizeof(struct slab_tls)*(UMALLOC_SHIFT_HIGH+1))

struct tls_freelist {
	int nr;
	void *tail;
	void *head;
	struct vpage *page;
};

struct slab_tls {
	struct vpage *page; /*slab对象页*/
	struct tls_freelist freelist; /*缓存释放对象*/
};

struct cache_node {
	spinlock_t lock; /**< Protect partial list and nr_partial */
	atomic_t nr_slab; /**< 包含的 slab 对象的个数*/
	uint32_t nr_partial; /**< 部分使用的 slab 对象的个数*/
	struct list_head partial; /**< 部分使用的 slab 对象的链表，连接 page->lru 节点 */
	struct list_head full;
};

/*
 * Slab cache management.
 * 1. 管理的对象需要使用RCU释放，对象实际空间至少为2个机器字节
 * align >= 0 && offset >= objsize
 * [ objsize ][align][ meta(ptr) ]
 * |<----offset---->|<---next--->|
 * |<------------size----------->|
 * 2. 否则，元数据与真实数据重叠
 * align >= 0 && offset == 0
 * [ objsize/meta ][align]
 * |<--------next------->|
 * |<-------size-------->|
 */
struct cache_objpool {
	union {
		long nr_version;
		struct {
			uint32_t nr:16;
			uint32_t verison:16;
		};
	};
	void *head;
} __aligned_double_cmpxchg;

struct umem_cache_s {
	struct cache_node node;
	struct vpage *slab_page; /**< 不使用线程私有缓存时，作为slab描述符缓存*/

	uint16_t flags;
	int16_t index; 		/**< 如果为负数则不使用线程缓存*/
	uint32_t size;		/**< 每个对象实际占用的空间大小 */
	uint32_t objsize;	/**< 用户请求的分配的对象大小 */
	int32_t order; 		/**< 扩展时每次分配的页数*/

	/* Allocation and freeing of slabs */
	uint32_t objects;	/**< Number of objects in slab */
	uint32_t inuse;		/**< 可使用的对象长度，也是到元数据的偏移 Offset to metadata */
	uint32_t align;		/* Alignment */
	uint32_t refcount;	/**< Refcount for slab cache destroy */
	uint32_t defrag_ratio; /**< 回收率*/

	int32_t objpool_cap;
	/*index == -1 的分配器，使用无锁栈缓存*/
	struct cache_objpool objpool;

	struct list_head list;	/* List of slab caches */
	const char *name;	/* Name (only for display!) */
};

#define cache_full(s) (&(s)->node.full)
#define cache_partial(s) (&(s)->node.partial)
#define has_partials(s) ((s)->node.nr_partial)

#define for_each_partial(p, s)	\
	list_for_each_entry((p), cache_partial((s)), lru)

static void *cache_alloc(struct umem_cache_s *);
static inline void cache_free(struct umem_cache_s *, const void *);

static void *slab_alloc(struct umem_cache_s *, struct vpage *);
static inline void slab_free(struct umem_cache_s*, struct vpage*, const void *);

#ifdef SLAB_DEBUG
/*一些统计信息*/
struct slab_acc {
	atomic_t nr_pages;
	atomic_t nr_slabs;
	atomic_t new_slabs;
	atomic_t discard_slabs;
	atomic_t nr_allocs;
	atomic64_t alloc_size;
};

static struct slab_acc slab_acc = {
	ATOMIC_INIT(0), ATOMIC_INIT(0), ATOMIC_INIT(0),
	ATOMIC_INIT(0), ATOMIC_INIT(0), ATOMIC64_INIT(0),
};

# define SLAB_BUG_ON(x) BUG_ON((x))
static inline void inc_new_slab(int32_t x)
{
	atomic_inc(&slab_acc.nr_slabs);
	atomic_inc(&slab_acc.new_slabs);
	atomic_add(1U << (x), &slab_acc.nr_pages);
}
static inline void inc_discard_slab(int32_t x)
{
	atomic_dec(&slab_acc.nr_slabs);
	atomic_inc(&slab_acc.discard_slabs);
	atomic_sub(1U << (x), &slab_acc.nr_pages);
}
static void add_alloc_size(size_t l)
{
	if (slab_state < SLAB_UP)
		return;
	atomic_inc(&slab_acc.nr_allocs);
	atomic64_add(l, &slab_acc.alloc_size);
}
static void sub_alloc_size(size_t l)
{
	if (slab_state < SLAB_UP)
		return;
	atomic_dec(&slab_acc.nr_allocs);
	atomic64_sub(l, &slab_acc.alloc_size);
}
static inline void slab_acc_check(void)
{
	size_t v;
	v = atomic_read(&slab_acc.nr_allocs);
	if (skp_unlikely(v))
		log_warn("memory leak %d ...", v);
	v = atomic64_read(&slab_acc.alloc_size);
	if (skp_unlikely(v))
		log_warn("memory leak %.4lf MB ...", (double)v/(1024*1024));
	log_info("SLAB ACC : new slabs [%d], discard slabs [%d]",
		atomic_read(&slab_acc.new_slabs), atomic_read(&slab_acc.discard_slabs));
}

#else
# define SLAB_BUG_ON(x)
# define inc_new_slab(x)
# define inc_discard_slab(x)
# define add_alloc_size(l)
# define sub_alloc_size(l)
# define slab_acc_check()

#endif

#ifdef SLAB_LEAK_CHECK
/*内存泄漏模块*/
struct leak_node {
	int line;
	void *addr;
	const char *file;
	struct rbt_node node;
};

static DEFINE_MUTEX(leak_lock);
static struct umem_cache_s *leak_allocater;
static const struct rbtree_ops leak_tree_ops;
static DEFINE_RBTREE(leak_tree, &leak_tree_ops);

static int leak_cmpkey(struct rbt_node*, const void *);
static int leak_cmpnode(struct rbt_node*, struct rbt_node*);

static const struct rbtree_ops leak_tree_ops = {
	.compare_key = leak_cmpkey,
	.compare_node = leak_cmpnode,
};

#define node2leak(p) container_of((p), struct leak_node, node)
#define cache2leak(p) container_of((p), struct leak_node, cache)

static int leak_cmpkey(struct rbt_node* p, const void *k)
{
	struct leak_node *node = node2leak(p);
	return node->addr == k ? 0 : (node->addr > k ? 1 : -1);
}

static int leak_cmpnode(struct rbt_node * _old, struct rbt_node* _new)
{
	struct leak_node *old = node2leak(_old);
	struct leak_node *new = node2leak(_new);
	return old->addr == new->addr ? 0 : (old->addr > new->addr? 1 : -1);
}

static inline void *alloc_leakn(void)
{
	struct umem_cache_s *s;
	if (skp_unlikely(!READ_ONCE(leak_allocater))) {
		/*如果实现没有错误，即使重复分配，也应该是同一个描述符*/
		s=umem_cache_create("leak_cache",sizeof(struct leak_node),32,SLAB_PANIC);
		BUG_ON(!s);
		s = xchg_ptr(&leak_allocater, s);
		umem_cache_destroy(s);
	}
	return cache_alloc(leak_allocater);
}

static inline void free_leakn(void *ptr)
{
	cache_free(leak_allocater, ptr);
}

static inline const char *get_filename(const char *file)
{
	const char *__ptr;
	return (__ptr = strrchr(file, '/')) ? (__ptr + 1) : file;
}

static void* leak_insert(void *p, const char *file, int line)
{
	struct rbt_node *node;
	struct leak_node *leakn, *r;

	if (skp_unlikely(!p))
		return p;
	if (skp_unlikely(!file))
		return p;
	if (skp_unlikely(READ_ONCE(slab_state) < SLAB_COMP))
		return p;

	leakn = alloc_leakn();
	leakn->addr = p;
	leakn->file = file;
	leakn->line = line;

	mutex_lock(&leak_lock);
	node = rbtree_insert_node(&leak_tree, &leakn->node);
	if (skp_unlikely(node)) {
		r = node2leak(node);
		struct vpage *page = virt_to_head_page(r->addr);
		log_error("DUPLICATE ALLOCATED : SOURCE [%s:%d,%p] NEW [%s:%d,%p], "
			"PAGE [%u]",
			get_filename(r->file), r->line, r->addr,
			get_filename(leakn->file), leakn->line, leakn->addr,
			page->inuse);
		BUG();
	}
	mutex_unlock(&leak_lock);
	return p;
}

static void leak_remove(const void *p)
{
	struct rbt_node *node;
	if (skp_unlikely(!p))
		return;
	mutex_lock(&leak_lock);
	node = rbtree_remove(&leak_tree, p);
	if (skp_unlikely(!node)) {
		log_error("POINTER BEING FREED WAS NOT ALLOCATED : %p", p);
		BUG();
	}
	mutex_unlock(&leak_lock);
	free_leakn(node2leak(node));
}

static void leak_check(void)
{
	const char *c;
	size_t l, nr = 0, tl = 0;
	struct leak_node *leakn, *next;

	mutex_lock(&leak_lock);
	rbtree_preorder_for_each_entry_safe(leakn, next, RBTREE_ROOT(&leak_tree),
			node) {
		l = usize(leakn->addr);
		nr++;
		tl += l;

		if (l < (1UL<<10)) {
			c = "B";
		} else if (l < (1UL<<20)) {
			c = "KB";
			l >>= 10;
		} else if (l < (1UL<<30)) {
			c = "MB";
			l >>= 20;
		} else {
			c = "GB";
			l >>= 30;
		}

		log_warn("memory leak : [%s:%d, %p:%zu%s]",
			get_filename(leakn->file), leakn->line, leakn->addr, l, c);
	}
	mutex_unlock(&leak_lock);
	if (nr > 0) {
		if (tl < (1UL<<10)) {
			c = "B";
		} else if (tl < (1UL<<20)) {
			c = "KB";
			tl >>= 10;
		} else if (tl < (1UL<<30)) {
			c = "MB";
			tl >>= 20;
		} else {
			c = "GB";
			tl >>= 30;
		}
		log_warn("memory leaks : %zu, %zu%s", nr, tl, c);
	}
}
#else
# define leak_insert(p, f, l) ((void*)(p))
# define leak_remove(p) ((void)(p))
# define leak_check()
#endif


/********************************************************************
 *		Kmalloc subsystem
 *******************************************************************/
int slab_state = SLAB_DOWN;
/*线程私有键*/
static pthread_key_t slabtls_key;
/*每个位一个 通用 cache 0 - 22 一共 23 个
 * 0 下标为 umem_cache_node 的专用分配器
 * 1、2 为 96、192 大小的非 2 幂次方 大小的分配器
 * 3~22 为 8 B ~ 4 MB 的 2 幂次方增长的分配器
 */
static struct umem_cache_s *umalloc_caches[UMALLOC_SHIFT_HIGH+1] __cacheline_aligned;
/*线程私有缓存指针*/
static __thread struct slab_tls *slab_tls = NULL;
/*回收tls中的缓存页*/
static void slabtls_reclaim(struct slab_tls *tls);
/*释放tls*/
static void slabtls_release(void *ptr);
/*装载tls*/
static __always_inline void *slabtls_acquire(int index);

/* A list of all slab caches on the system */
static DEFINE_RWSEM(slub_lock);
static LIST__HEAD(slab_caches);

#define for_each_cache(p) \
	list_for_each_entry((p), &slab_caches, list)

#define for_each_cache_reverse(p) \
	list_for_each_entry_reverse((p), &slab_caches, list)

/*从无锁链表分配*/
static inline void *lockless_alloc(struct vpage *page)
{
	void **object = NULL;
	if (skp_likely(page && page->lockless_freelist)) {
		object = page->lockless_freelist;
		page->lockless_freelist = object[0];
	}
	return object;
}

static inline void lockless_free(struct vpage *page, const void *ptr)
{
	void **object = (void**)ptr;
	object[0] = page->lockless_freelist;
	page->lockless_freelist = object;
}

/*从有锁链表分配对象，并且所有剩余对象装载到无锁链表中*/
static inline void *freelist_alloc(struct vpage *page)
{
	void **object = page->freelist;
	/*分配一个对象后，剩余对象全部转移给per-CPUs无锁链表来管理对象分配
	 * 1. page->freelist 中的对象可能从 __slab_free() 中释放（补充）而来*/
	SLAB_BUG_ON(!page->freelist);
	SLAB_BUG_ON(page->lockless_freelist);
	page->lockless_freelist = object[0];
	/*page->inuse 设置为最值，表示 page->freelist 中对象全部分配完毕*/
	page->inuse = page->slab->objects;
	page->freelist = NULL;
	return object;
}

static inline void freelist_free(struct vpage *page, const void *head,
		const void *tail, int nr)
{
	tail = tail?:head;
	void **object = (void**)tail;
	/* 然后归还到对象链表中*/
	object[0] = page->freelist;
	page->freelist = (void*)head;
	page->inuse-=nr;

//#ifdef SLAB_DEBUG
#if 0
	nr = 0;
	object = page->freelist;
	do {
		nr++;
	} while((object = object[0]));
	SLAB_BUG_ON((nr+page->inuse)!=page->slab->objects);
#endif
}

static inline int get_index(size_t size)
{
	int index;
	if (UMALLOC_MIN_SIZE <= 64 && size == 96)
		return 1;
	if (UMALLOC_MIN_SIZE <= 128 && size == 192)
		return 2;
	index = order_base_2(ALIGN(size?:1, sizeof(long)));
	if (skp_unlikely(index > UMALLOC_SHIFT_HIGH))
		return -1;
	return index;
}

static inline bool need_discard(struct vpage *page)
{
	uint32_t nr;
	struct umem_cache_s *s = page->slab;
	if (skp_unlikely(s->flags & SLAB_NONDISCARD))
		return false;
	/*正在收缩*/
	if (skp_unlikely(READ_ONCE(s->flags) & __SLAB_SHRINKING))
		return true;
	/*对象大小超过阈值都不缓存*/
	nr = s->node.nr_partial;
	/*对象数目小说明对象大*/
	if (s->objects <= SLAB_MIN_OBJECTS && nr > UMALLOC_MIN_PARTIAL)
		return true;
	if (s->objects > SLAB_MIN_OBJECTS && nr > UMALLOC_MAX_PARTIAL)
		return true;
	return false;
}

/*frozen 标志表示 slab 被装载到 某种缓存中了，不能被释放。*/
static inline bool SlabFrozen(struct vpage *page)
{
	return PageActive(page);
}

static inline void SetSlabFrozen(struct vpage *page)
{
	__SetPageActive(page);
}

static inline void ClearSlabFrozen(struct vpage *page)
{
	smp_wmb();
	__ClearPageActive(page);
	smp_mb();
}

static inline void slab_lock(struct vpage *page)
{
	page_lock(page);
}

static inline void slab_unlock(struct vpage *page)
{
	page_unlock(page);
}

static inline bool slab_trylock(struct vpage *page)
{
	return page_trylock(page);
}

static inline void init_cache_lock(struct umem_cache_s *s)
{
	spin_lock_init(&s->node.lock);
}

static inline bool cache_islocked(struct umem_cache_s *s)
{
	return spinlock_is_locked(&s->node.lock);
}

static inline bool cache_trylock(struct umem_cache_s *s)
{
	return spin_trylock(&s->node.lock);
}

static inline void cache_lock(struct umem_cache_s *s)
{
	spin_lock(&s->node.lock);
	SLAB_BUG_ON(!(s)->node.nr_partial && !list_empty(&(s)->node.partial));
	SLAB_BUG_ON((s)->node.nr_partial && list_empty(&(s)->node.partial));
}

static inline void cache_unlock(struct umem_cache_s *s)
{
	SLAB_BUG_ON(!(s)->node.nr_partial && !list_empty(&(s)->node.partial));
	SLAB_BUG_ON((s)->node.nr_partial && list_empty(&(s)->node.partial));
	spin_unlock(&s->node.lock);
}

/*
 * 尾部存放完全空闲的SLAB对象
 */
static inline void __add_partial_tail(struct umem_cache_s *s,struct vpage *page)
{
	s->node.nr_partial++;
	SLAB_BUG_ON(page->slab != s);
	SLAB_BUG_ON(page_on_list(page));
	add_page_to_list_tail(page, cache_partial(s));
}

/*
 * 头部存放部分空闲的SLAB对象
 */
static inline void __add_partial(struct umem_cache_s *s, struct vpage *page)
{
	s->node.nr_partial++;
	SLAB_BUG_ON(page->slab != s);
	SLAB_BUG_ON(page_on_list(page));
	add_page_to_list(page, cache_partial(s));
}

/*
 * 尾部存放完全空闲的SLAB对象
 */
static inline void add_partial_tail(struct umem_cache_s *s, struct vpage *page)
{
	cache_lock(s);
	__add_partial_tail(s, page);
	cache_unlock(s);
}

/*
 * 头部存放部分空闲的SLAB对象
 */
static inline void add_partial(struct umem_cache_s *s, struct vpage *page)
{
	cache_lock(s);
	__add_partial(s, page);
	cache_unlock(s);
}

static inline void __remove_partial(struct umem_cache_s *s, struct vpage *page)
{
	s->node.nr_partial--;
	SLAB_BUG_ON(page->slab != s);
	SLAB_BUG_ON(!page_on_list(page));
	del_page_from_list(page);
}

static inline void remove_partial(struct umem_cache_s *s, struct vpage *page)
{
	cache_lock(s);
	__remove_partial(s, page);
	cache_unlock(s);
}

#ifdef SLAB_DEBUG
static inline void __add_full(struct umem_cache_s *s, struct vpage *page)
{
	SLAB_BUG_ON(page->slab != s);
	SLAB_BUG_ON(page_on_list(page));
	add_page_to_list(page, cache_full(s));
}

static inline void __remove_full(struct umem_cache_s *s, struct vpage *page)
{
	SLAB_BUG_ON(page->slab != s);
	SLAB_BUG_ON(!page_on_list(page));
	del_page_from_list(page);
}

static inline void add_full(struct umem_cache_s *s, struct vpage *page)
{
	cache_lock(s);
	__add_full(s, page);
	cache_unlock(s);
}

static inline void remove_full(struct umem_cache_s *s, struct vpage *page)
{
	cache_lock(s);
	__remove_full(s, page);
	cache_unlock(s);
}
#else
# define __add_full(s, p)
# define add_full(s, p)
# define __remove_full(s, p)
# define remove_full(s, p)
#endif

/* Loop over all objects in a slab */
#define for_each_object(__p, __s, __start, __end) \
	for (__p = (__start); __p < (__end); __p += __s)

static inline bool lockless_cache(struct umem_cache_s *s)
{
	return !!(s->index > -1);
}

static inline void put_slab_page(struct vpage *page)
{
	SLAB_BUG_ON(!page);
	SLAB_BUG_ON(!page->slab);
	bool put = !lockless_cache(page->slab);
	slab_unlock(page);
	if (put)
		put_page(page);
}

static inline void slab_page_check(struct vpage *page, struct umem_cache_s *s)
{
	SLAB_BUG_ON(!page);
	SLAB_BUG_ON(page != s->slab_page);
	SLAB_BUG_ON(page->slab != s);
	SLAB_BUG_ON(!PageSlab(page));
	SLAB_BUG_ON(page_testone(page));
}

/*由于可能有多个线程操作非 lockless 的 slab ，所以其获取过程有些复杂*/
static struct vpage *get_slab_page(struct umem_cache_s *s)
{
	struct vpage *page = s->slab_page;
	if (skp_unlikely(!page || !SlabFrozen(page)))
		return NULL;

	if (skp_unlikely(!get_page(page)))
		return NULL;

	slab_lock(page);
	/* 这个间隙 s->slab_page 被释放，
	 * 但 page 又被其他的 slab 分配并使用，
	 * 所以 SlabFrozen() 可以为真，必须再次确保 s->slab_page 未变*/
	if (skp_unlikely(!SlabFrozen(page) || page != s->slab_page)) {
		put_slab_page(page);
		return NULL;
	}
	slab_page_check(page, s);
	return page;
}

static inline void set_freepointer(uintptr_t object, uintptr_t fp)
{
	*(uintptr_t*)object = fp;
}

static inline uint32_t calculate_alignment(uint32_t flags, uint32_t align,
		uint32_t size)
{
	if ((flags & SLAB_HWCACHE_ALIGN) && size > CACHELINE_BYTES / 2)
		return max_t(uint32_t, align, CACHELINE_BYTES);

	if (align < SLAB_MINALIGN)
		return SLAB_MINALIGN;

	return ALIGN(align, sizeof(void *));
}

static int32_t slab_order(uint32_t size, uint32_t min_objs, int32_t max_order,
		int fract_leftover)
{
	uint32_t rem;
	unsigned long slab_size;
	int32_t order = max_t(int32_t, SLAB_MIN_ORDER,
						fls(min_objs * size - 1) - VPAGE_SHIFT);

	while (order <= max_order) {
		slab_size = VPAGE_SIZE << order;
		if (slab_size >= min_objs * size) {
			rem = slab_size % size;
			/*闲置率小于 1/fract_leftover */
			if (rem <= slab_size / fract_leftover)
				break;
		}
		order++;
	}

	return order;
}

static int calculate_order(uint32_t size)
{
	uint32_t order, min_objs, fraction;

	min_objs = SLAB_MIN_OBJECTS;
	while (min_objs > 1) {
		fraction = 8;
		while (fraction >= 4) {
			order = slab_order(size, min_objs, SLAB_MAX_ORDER, fraction);
			if (order <= SLAB_MAX_ORDER)
				return order;
			fraction /= 2;
		}
		min_objs /= 2;
	}

	order = slab_order(size, 1, SLAB_MAX_ORDER, 1);
	if (order <= SLAB_MAX_ORDER)
		return order;

	order = slab_order(size, 1, MAX_ORDER, 1);
	if (order < MAX_ORDER)
		return order;

	return -1;
}
/*
 * calculate_sizes() determines the order and the distribution of data within
 * a slab object.
 */
static bool calculate_sizes(struct umem_cache_s *s)
{
	uint32_t flags = s->flags;
	uint32_t align = s->align;
	uint32_t size = s->objsize;

	size = ALIGN(size, sizeof(void *));
	s->inuse = size;

	/*计算对齐字节*/
	align = calculate_alignment(flags, align, s->objsize);

	size = ALIGN(size, align);
	s->size = size;
	s->order = 0;

	/*如果不是初始化 0 号缓存描述符，则需要计算 补充slab内存时的虚拟页数*/
	if (skp_likely(slab_state != SLAB_DOWN)) {
		s->order = calculate_order(size);
		if (s->order < 0)
			return false;
	}

	s->objects = (uint32_t)((VPAGE_SIZE << s->order) / size);

	if (WARN_ON(!s->objects))
		return false;
	return true;
}

static void init_umalloc_cache(struct umem_cache_s *s)
{
	struct cache_node *n = &s->node;

	memset(s, 0, sizeof(*s));
	
	s->index = -1;
	s->order = -1;
	s->refcount = 1;
	s->objpool.head = NULL;
	s->objpool.nr_version = 0;
	s->defrag_ratio = 100;
	s->align = UMALLOC_MINALIGN;
	s->objpool_cap = TLS_FREELIST_CAP * 2;

	init_cache_lock(s);
	INIT_LIST_HEAD(&s->list);
	INIT_LIST_HEAD(&n->full);
	INIT_LIST_HEAD(&n->partial);
	atomic_set(&n->nr_slab, 0);
}

static struct umem_cache_s *early_cache_create(struct umem_cache_s *tmp)
{
	struct vpage *page;
	struct umem_cache_s *s;

	SLAB_BUG_ON(tmp->objsize < sizeof(*tmp));

	/*已经可以分配描述符了，分配一个描述符，并将临时描述符中的数据
	 *移动到正式的描述符中。
	 */
	s = cache_alloc(tmp);
	SLAB_BUG_ON(!s);

	cache_lock(tmp);
	SLAB_BUG_ON(atomic_read(&tmp->node.nr_slab) != 1);
	SLAB_BUG_ON(!list_empty(&tmp->node.full));
	SLAB_BUG_ON(!list_empty(&tmp->node.partial));

	/*拷贝所有属性*/
	memcpy(s, tmp, sizeof(*s));
	init_cache_lock(s);
	/*只分配了一个 slab 块并装载到了线程变量中，所有链表全为空*/
	INIT_LIST_HEAD(&s->node.full);
	INIT_LIST_HEAD(&s->node.partial);

	cache_lock(s);
	/*替换slab描述符虚地址所指向的页描述符中的临时 slab 指针 为正式描述符指针*/
	page = compound_head(virt_to_page(s));
	slab_lock(page);
	SLAB_BUG_ON(page->slab != tmp);
	page->slab = s;
	slab_unlock(page);
	cache_unlock(s);
	cache_unlock(tmp);

	return s;
}

/*初始化 cache 对象，然后添加到全局链表中*/
static struct umem_cache_s * __create_cache(const char *name, uint32_t size,
		uint32_t align, uint32_t flags)
{
	/*0号分配器描述符使用栈变量作为临时存储，初始化后在替换*/
	struct umem_cache_s tmp, *s = &tmp;

	if (skp_likely(slab_state != SLAB_DOWN)) {
		/*0号通用内存分配器可以工作了，动态分配描述符*/
		s = cache_alloc(umalloc_caches[0]);
		if (skp_unlikely(!s))
			return NULL;
	}

	init_umalloc_cache(s);

	s->name = name;
	s->align = align;
	s->objsize = size;
	s->flags = flags & ~__SLAB_SHRINKING;

	/*
	 * 1. 计算每个对象占用的大小和其元数据大小（一个指针），
	 * 2. 每次扩展分配的页数和能被初始化成多少个对象
	 * [ slab block                      ]
	 * |<-objsize->|<-align1->|<-align2->|
	 * |<----inuse----------->|
	 * |<----size----------------------->|
	 */
	if (!calculate_sizes(s))
		goto error;

	/*一次初始化，需要特别处理*/
	if (skp_unlikely(s == &tmp))
		s = early_cache_create(&tmp);

	if (skp_likely(slab_state >= SLAB_UP)) {
		s->name = uasprintf("%s-%u", name, size);
		log_info("create slab [%s] size=%u realsize=%u objects=%u order=%u "
			"flags=%x", s->name, size, s->size, s->objects, s->order, flags);
	}

	return s;

error:
	log_warn("can't create slab %s size=%u realsize=%u objects=%u order=%u"
		" flags=%x", s->name, size, s->size, s->objects, s->order, flags);
	return NULL;
}

static struct umem_cache_s * create_cache(const char *name, uint32_t size,
		uint32_t flags)
{
	struct umem_cache_s *s = __create_cache(name, size, 0, flags);
	if (skp_likely(s)) {
		down_write(&slub_lock);
		list_add(&s->list, &slab_caches);
		up_write(&slub_lock);
	}
	return s;
}

static void cache_release(void)
{
	leak_check();
	slab_acc_check();
}

static __always_inline void* slabtls_acquire(int index)
{
	int rc;
	struct slab_tls *tls = slab_tls;
	if (skp_likely(tls))
		goto done;

	SLAB_BUG_ON(!index);
	SLAB_BUG_ON(slab_state < SLAB_UP);

	tls = pthread_getspecific(slabtls_key);
	if (WARN_ON(tls))
		goto out;

	/*分配TLS*/
	tls = cache_alloc(umalloc_caches[0]);
	memset(tls, 0, TLS_ALLOCATER_OBJSIZE);

	/*设置TLS*/
	rc = pthread_setspecific(slabtls_key, tls);
	BUG_ON(rc);

out:
	slab_tls = tls;
done:
	return &tls[index];
}

void __umem_cache_init(void)
{
	int nr_s = 0;

	/*无锁栈*/
	BUILD_BUG_ON(sizeof(void*)!=sizeof(long));

	big_lock();
	if (READ_ONCE(slab_state) >= SLAB_UP) {
		big_unlock();
		return;
	}

	BUG_ON(pthread_key_create(&slabtls_key, slabtls_release));
	/*
	 * 1. 初始化 0 号通用缓存 用于分配 struct umem_cache_node 的通用 cache
	 *    以及线程TLS描述符
	 * 2. 每个 umem_cache_node 数据上的都分配了一个用于分配 slab 并
	 * 分配一个用于自身
	 */
	umalloc_caches[0] = create_cache("umem_cache/slab_tls", max_t(size_t,
					sizeof(struct umem_cache_s), TLS_ALLOCATER_OBJSIZE),
					SLAB_HWCACHE_ALIGN|SLAB_RECLAIM|SLAB_PANIC);
	/*0号缓存不使用线程私有缓存*/
	umalloc_caches[0]->index = -1;
	/*0号缓存永远不会被释放*/
	umalloc_caches[0]->refcount = -1;
	nr_s++;
	/* 标识 可以动态分配 umem_cache_node 结构体了 */
	slab_state = SLAB_PARTIAL;

	/* Caches that are not of the two-to-the-power-of size */
	if (UMALLOC_MIN_SIZE <= 64) {
		umalloc_caches[1] = create_cache("umalloc-96", 96, SLAB_RECLAIM);
		umalloc_caches[1]->index = 1;
		nr_s++;
	}

	if (UMALLOC_MIN_SIZE <= 128) {
		umalloc_caches[2] = create_cache("umalloc-192", 192, SLAB_RECLAIM);
		umalloc_caches[2]->index = 2;
		nr_s++;
	}

	/*
	 * 2^n (n >= 3)
	 */
	for (int i = UMALLOC_SHIFT_LOW; i <= UMALLOC_SHIFT_HIGH; i++) {
		umalloc_caches[i] = create_cache("umalloc-x", 1U << i, SLAB_RECLAIM);
		umalloc_caches[i]->index = i;
		nr_s++;
	}

	/*标识完全初始化*/
	WRITE_ONCE(slab_state, SLAB_UP);
	/*must be here, replace name by alloceted memory*/
	for (int i = UMALLOC_SHIFT_LOW; i <= UMALLOC_SHIFT_HIGH; i++) {
		if (i<10) {
			umalloc_caches[i]->name = uasprintf("umalloc-%u", 1U << i);
		} else if (i<20) {
			umalloc_caches[i]->name = uasprintf("umalloc-%uK", 1U << (i - 10));
		} else if (i<30) {
			umalloc_caches[i]->name = uasprintf("umalloc-%uM", 1U << (i - 20));
		} else {
			umalloc_caches[i]->name = uasprintf("umalloc-%uG", 1U << (i - 30));
		}
		sub_alloc_size(usize(umalloc_caches[i]->name));
	}

	/*注册销毁*/
	atexit(cache_release);
	WRITE_ONCE(slab_state, SLAB_COMP);

	/*验证内存泄漏模块*/
	leak_insert((void*)0x1UL, __FILE__, __LINE__);
	leak_remove((void*)0x1UL);

	big_unlock();

	for (int i = 0; i <= UMALLOC_SHIFT_HIGH; i++) {
		struct umem_cache_s *s = umalloc_caches[i];
		if (skp_unlikely(!s))
			continue;
		log_debug("create GensSlab [%s] size=%u realsize=%u objects=%u "
			"order=%u flags=%x", s->name, s->objsize, s->size, s->objects,
			s->order, s->flags);
	}
	/* Provide the correct umalloc names now that the caches are up */
	log_info("SLUB: Genslabs=%d, HWalign=%d, Order=%d-%d, MinObjects=%d",
		nr_s, CACHELINE_BYTES, SLAB_MIN_ORDER, SLAB_MAX_ORDER, SLAB_MIN_OBJECTS);
}

static void init_slab(struct umem_cache_s *s, struct vpage *page)
{
	uintptr_t p, last, start, end;

	start = (uintptr_t)page_to_virt(page);
	/*！！！注意宽度*/
	end = start + ((uintptr_t)s->objects) * s->size;
	SLAB_BUG_ON(end <= start);

	/*
	 * 对可用对象的 meta 区的字节偏移寻址转化为纯 void* 数组操作
	 * 初始化页的字段
	 */
	page->slab = s;
	page->inuse = 0;
	page->freelist = (void*)start;
	page->lockless_freelist = NULL;

	__SetPageSlab(page);
	/*
	 * 奇特的遍历设置，第一个对象被初始化了两次，一次为无效初始化
	 * 而最后一个对象却在循环外部初始化
	 */
	last = start;
	for_each_object(p, s->size, start, end) {
		/*设置每个对象包含的meta信息，指向下一个可用对象的起始位置
		 * +------+----+
		 * |offset|meta|--> next pointer
		 * +------+--+-+
		 */
		set_freepointer(last, p);
		last = p;
	}
	set_freepointer(last, 0);

	/*
	 * 初始化完毕后
	 *           +---+       +---+       +---+       +---+       +----->NULL
	 *          /    |      /    |      /    |      /    |      /
	 *  +------+====+v-----+====+v-----+====+v-----+====+v-----+====+
	 *  |offset|meta|offset|meta|offset|meta|offset|meta|offset|meta|
	 *  ^------+====+------+====+------+====+------+====+------+====+
	 *  |obj0       |obj1       |obj2       |obj3       |obj4       |
	 *  |
	 *  |      +--------+
	 *  +------|freelist| page
	 *         +--------+
	 *
	 * 使用时，每一个对象涉及的空间都看成 void* 的数组，而把对象起始地址
	 * 看成 void* obj[]，下一个对象的地址就存在 void *obj[page->offset]
	 * 中（meta区）
	 */
}

/** 分配页并构造对象链表，然后初始化页描述符中相应的字段*/
static __always_inline struct vpage *new_slab(struct umem_cache_s *s)
{
	int flags = 0;
	struct vpage *page;

	flags |= s->order ? __GFP_COMP : 0;
	flags |= s->flags & SLAB_RECLAIM ? __GFP_RECLAIM : 0;
	/*分配页
	 *标识正在分配，防止由于页回收导致递归调用或不必要的排序
	 */
	page = __alloc_pages(flags, s->order);
	if (skp_unlikely(!page))
		return NULL;

	init_slab(s, page);
	inc_new_slab(s->order);
	atomic_inc(&s->node.nr_slab);

#ifdef SLAB_DEBUG
	log_debug("slloc new slab : %p, %p[%s]\n"
			 "current statistic of slab : slabs [%d], pages [%d]",
			 page, s, s->name, atomic_read(&slab_acc.nr_slabs),
			 atomic_read(&slab_acc.nr_pages));
#endif
	return page;
}

static __always_inline void discard_slab(struct umem_cache_s *s,
		struct vpage *page, bool direct)
{
	SLAB_BUG_ON(page->slab != s);

	atomic_dec(&s->node.nr_slab);

	page->inuse = 0;
	page->slab = NULL;
	page->freelist = NULL;
	page->lockless_freelist = NULL;
	__ClearPageSlab(page);

	if (direct) {
		___free_pages(page, s->order);
	} else {
		__free_pages(page, s->order);
	}

	inc_discard_slab(s->order);
#ifdef SLAB_DEBUG
	log_debug("discard slab : %p, %p[%s]\n"
			 "current statistic of slab : slabs [%d], pages [%d]",
			 page, s, s->name, atomic_read(&slab_acc.nr_slabs),
			 atomic_read(&slab_acc.nr_pages));
#endif
}

/**
 * 1. 将对象从percpu中无锁链表中归还到空闲链表中，并重置 per-CPU 页槽位，
 * 2. 归还slab页到部分链表中，
 * 3. 或可能移动到全部使用链表中，
 * 4. 解除页锁
 * !!! page 必须有额外的引用计数
 */
static void deactivate_slab(struct umem_cache_s *s, struct vpage *page,
		struct slab_tls *tls, bool direct)
{
	bool discard = false;
	void **tail = page->lockless_freelist;

	SLAB_BUG_ON(!page);
	SLAB_BUG_ON(page->slab != s);
	SLAB_BUG_ON(!SlabFrozen(page));
	SLAB_BUG_ON(!lockless_cache(s) && page_count(page) < 2);

	if (skp_unlikely(tail)) {
		int nr = 1;
		while (tail[0]) {
			nr++;
			tail = tail[0];
		}
		freelist_free(page, page->lockless_freelist, tail, nr);
		page->lockless_freelist = NULL;
	}

	/*马上要归还到部分链表，所以解除 per-CPU 缓存*/
	if (lockless_cache(s)) {
		SLAB_BUG_ON(!tls);
		SLAB_BUG_ON(tls[s->index].page != page);
		tls[s->index].page = NULL;
	} else if (skp_likely(s->slab_page == page)) {
		cache_lock(s);
		SLAB_BUG_ON(s->slab_page != page);
		WRITE_ONCE(s->slab_page, NULL);
		cache_unlock(s);
	} else {
		/*解除页锁*/
		log_warn("page has been unload by others already : %p, %p(%u)",
			page, s, s->size);
		goto out;
	}
	/*解锁页，并归还到链表中*/
	ClearSlabFrozen(page);
	if (skp_likely(page->inuse)) {
		/*所拥有的对象尚未释放（未归还）*/
		if (skp_unlikely(page->freelist)) {
			/* 持有的对象尚未完全被分配 线程退出时可能走此路径
			 * !!! 如果是非 lockless 类型的，另一个线程可能以获取 s->node.lock
			 * 此刻将被阻塞，直到其他线程装载 s->slab_page 成功
			 */
			add_partial(s, page);
		} else {
			add_full(s, page);
		}
	} else {
		SLAB_BUG_ON(!page->freelist);
		discard = need_discard(page);
		/*没有任何对象被分配，根据配置来选择释放 slab 还是暂存在部分链表的末尾 */
		if (!discard)
			add_partial_tail(s, page);
	}
out:
	put_slab_page(page);
	/*一定要解锁释放，防止性能损失*/
	if (discard)
		discard_slab(s, page, direct);

	return;
}

/*
 * Lock slab and remove from the partial list.
 *
 * Must hold list_lock.
 */
static bool lock_and_freeze_slab( struct umem_cache_s *s, struct vpage *page)
{
	/*
	 * 加锁可能不成功，因为其他路径正在释放SLAB中的对象
	 * 因为 cache 的锁和 page 的锁经常无序的嵌套，
	 * 所以必须以 trylock 的方式加锁
	 */
	if (!slab_trylock(page))
		return false;

	SetSlabFrozen(page);
	/*load to cache*/
	SLAB_BUG_ON(page->slab != s);
	if (lockless_cache(s)) {
		/*TODO:如果没有初始化，则不装载*/
		SLAB_BUG_ON(slab_tls[s->index].page);
		slab_tls[s->index].page = page;
	} else {
		SLAB_BUG_ON(s->slab_page);
		__get_page(page);
		WRITE_ONCE(s->slab_page, page);
	}
	return true;
}

/*
 * Try to allocate a partial slab from a specific node.
 */
static struct vpage *get_partial_lockless(struct umem_cache_s *s)
{
	struct vpage *page;

	if (!has_partials(s))
		return NULL;
try:
	cache_lock(s);
	/*遍历部分链表，获取可用的slab对象，然后从链表中移除*/
	for_each_partial(page, s) {
		if (lock_and_freeze_slab(s, page)) {
			__remove_partial(s, page);
			goto out;
		}
	}
	/*有部分空闲的页，但是可能有其他路径正在释放其上的内存块，加锁就会失败，
	 *继续尝试防止分配过多的slab页，造成太多的碎片*/
	if (has_partials(s)) {
		cache_unlock(s); goto try;
	}

	page = NULL;
out:
	cache_unlock(s);
	return page;
}

static struct vpage *get_partial(struct umem_cache_s *s)
{
	struct vpage *page;

	if (lockless_cache(s))
		return get_partial_lockless(s);

	/*试图装载第一个，可能为空*/
try:
	cache_lock(s);
	page = s->slab_page;
	if (skp_unlikely(page)) {
		/*其他线程可能已锁定页，并正打算锁定 cache_s，所以必须尝试性加锁*/
		if (!slab_trylock(page)) {
			cache_unlock(s); goto try;
		}
		page = get_page(page);
		slab_page_check(page, s);
		/*一定是被装载的页或未被完全卸载的页，
		 *另一个线程也可以拿到这个页*/
		goto out;
	}

	/*遍历部分链表，获取可用的slab对象，然后从链表中移除*/
	for_each_partial(page, s) {
		if (lock_and_freeze_slab(s, page)) {
			BUG_ON(page->lockless_freelist);
			__remove_partial(s, page);
			goto out;
		}
	}

	if (has_partials(s)) {
		cache_unlock(s); goto try;
	}

	/*小心死锁*/
	cache_unlock(s);

	page = new_slab(s);
	if (skp_unlikely(!page))
		return NULL;

	cache_lock(s);
	/*解锁 cache 的期间，其他线程进行了装载，需要再次确认*/
	if (skp_likely(!s->slab_page)) {
		BUG_ON(!lock_and_freeze_slab(s, page));
		goto out;
	} else if (!need_discard(page)) {
		__add_partial_tail(s, page);
		page = NULL;
	}
	cache_unlock(s);

	if (page)
		discard_slab(s, page, true);

	goto try;
out:
	cache_unlock(s);
	return page;
}

static struct vpage *get_empty(struct umem_cache_s *s)
{
	struct vpage *page, *old;

	if (!lockless_cache(s))
		return NULL;

	/* 需要分配并初始化一个新的 slab 对象页
	 * todo : 分配失败，设置一个全局的紧急标志*/
	page = new_slab(s);

	/*需要检查 tls 是否已经装载，new_slab 可能也使用了 umalloc()
	 * @see pgtls_acquire()
	 */
	old = slab_tls[s->index].page;
	if (skp_unlikely(old))
		goto exist;

load:
	if (skp_unlikely(!page))
		return NULL;
	BUG_ON(!lock_and_freeze_slab(s, page));
	return page;

exist:
	slab_lock(old);
	deactivate_slab(s, old, slab_tls, false);

	old = get_partial_lockless(s);
	if (skp_likely(old)) {
		if (skp_likely(page))
			discard_slab(s, page, false);
		return old;
	}
	goto load;
}

/*
 * TODO:渐进式初始化对象链表虚地址
 */
static void *slab_alloc(struct umem_cache_s *s, struct vpage *page)
{
	void *object;

	if (skp_unlikely(!page))
		goto new_slab;

	/*查看 freelist 需要加锁，因为会与 slab_free() 产生竞争*/
	if (lockless_cache(s))
		slab_lock(page);

load_slab:
	/*
	 * 对于非系统分配器
	 * 实际的装载由其他路径执行，并且释放了一些数据
	 * 尝试从无锁链表分配
	 */
	object = lockless_alloc(page);
	if (object)
		goto out;

	if (!page->freelist)
		goto another_slab;

	/*从 freelist 分配，并装载 lockless*/
	object = freelist_alloc(page);

out:
	put_slab_page(page);

	return object;
another_slab:
	deactivate_slab(s, page, slab_tls, false);

new_slab:
	/*尝试从 slab 的部分链表中获取空闲的 slab 页，并从链表中剥离*/
	page = get_partial(s);
	if (!page)
		page = get_empty(s);
	if (skp_unlikely(!page))
		goto fail;

	goto load_slab;

fail:
	if (skp_unlikely(s->flags & SLAB_PANIC)) {
		log_error("out of memory");
		BUG();
	}
	return NULL;
}

static void __slab_free(struct vpage *page, const void *head, const void *tail,
		int nr, bool direct)
{
	void *prior; /*为空表示释放此对象前，含有的对象被全部分配*/
	struct umem_cache_s *s = page->slab;

	slab_lock(page);
	/*一旦确定了被释放的对象是属于 page 描述的 slab,
	 *则 page 就不可能失效，因为还有对象没有被释放*/
	prior = READ_ONCE(page->freelist);
	freelist_free(page, head, tail, nr);

	/*被装载到了per-thread 或 s->slab_page 结构中，不能被释放，也不能移动*/
	if (SlabFrozen(page))
		goto out_unlock;

	/*管理的任何对象都没有被分配，释放slab*/
	if (skp_unlikely(!page->inuse)) {
		if (need_discard(page))
			goto slab_empty;
	}

	/* prior 非空 一定在 partial 中, prior 为空 一定在 full 中 */
	if (skp_unlikely(!prior)) {
		remove_full(s, page);
		add_partial_tail(s, page);
	} else {
		SLAB_BUG_ON(!page_on_list(page));
	}

out_unlock:
	slab_unlock(page);

	return;

slab_empty:
	SLAB_BUG_ON(page->lockless_freelist);
	if (skp_unlikely(prior)) {
		/*本次释放的是最后一个对象*/
		remove_partial(s, page);
	} else {
		/*一次性释放，一定在full中*/
		remove_full(s, page);
	}
	slab_unlock(page);

	discard_slab(s, page, direct);
	return;
}

static inline void *slab_alloc_lockless(struct umem_cache_s *s)
{
	struct slab_tls *tls = slabtls_acquire(s->index);
	void *obj = lockless_alloc(tls->page);
	if (skp_unlikely(!obj))
		obj = slab_alloc(s, tls->page);
	return obj;
}

static void *cache_alloc(struct umem_cache_s *s)
{
	void *obj;
	struct vpage *page;

	if (lockless_cache(s)) {
		obj = slab_alloc_lockless(s);
	} else {
		/*slab_page 被多个线程操作，使用时需要加锁*/
		page = get_slab_page(s);
		obj = lockless_alloc(page);
		if (skp_likely(obj)) {
			put_slab_page(page);
		} else {
			obj = slab_alloc(s, page);
		}
	}

	if (skp_likely(obj))
		add_alloc_size(s->size);

	return obj;
}

static inline void tls_freelist_flush(struct tls_freelist *fl, bool direct)
{
	if (!fl->nr)
		return;
	__slab_free(fl->page, fl->head, fl->tail, fl->nr, direct);
	fl->nr = 0;
	fl->page = fl->head = fl->tail = NULL;
}

/*per-thread 缓存，然后批量释放 */
static inline void tls_cache(struct slab_tls *tls, struct vpage *page,
		const void *x, bool direct)
{
	struct tls_freelist *fl = &tls->freelist;
	/*不同的页 或 缓存太多*/
	if (fl->page != page || fl->nr>=TLS_FREELIST_CAP)
		tls_freelist_flush(fl, direct);

	/*压栈缓存*/
	*(void**)x = fl->head;
	fl->nr++;
	fl->page = page;
	fl->head = (void*)x;
	if (skp_unlikely(!fl->tail))
		fl->tail = fl->head;
}

static inline void slab_free(struct umem_cache_s *s, struct vpage *page,
		const void *x)
{
	/*当前线程分配，当前线程释放，且 slab 描述符没有改变*/
	if (lockless_cache(s)) {
		struct slab_tls *tls = slabtls_acquire(s->index);
		if (tls->page == page) {
			lockless_free(page, x);
		} else {
			tls_cache(tls, page, x, false);
		}
	} else {
		/*分配可能从per-thread上得到，但释放时却在另外的线程中，直接释放到SLAB上*/
		__slab_free(page, x, NULL, 1, false);
	}
	sub_alloc_size(s->size);
}

static void objpool_flush(struct umem_cache_s *s, bool direct)
{
	long nr_version = 0;
	struct slab_tls tls;
	struct cache_objpool opool;
	struct cache_objpool *pool = &s->objpool;
	struct tls_freelist *fl = &tls.freelist;

	if (lockless_cache(s))
		return;

	/*是否池中对象*/
	do {
		opool.head = READ_ONCE(pool->head);
		opool.nr_version = READ_ONCE(pool->nr_version);
		if (!opool.nr)
			return;
		nr_version = opool.nr_version;
		/*修改新值*/
		opool.nr = 0;
		opool.verison++;
	} while (!cmpxchg_double(&pool->nr_version, &pool->head,
					nr_version, opool.head, opool.nr_version, NULL));

	opool.nr_version = nr_version;
	static_mb();
	SLAB_BUG_ON(!opool.nr);
	SLAB_BUG_ON(!opool.head);

	fl->nr = 0;
	fl->page = fl->head = fl->tail = NULL;

	while (opool.head) {
		void **obj = (void**)opool.head;
		opool.nr--;
		opool.head = obj[0];
		tls_cache(&tls, virt_to_head_page(obj), obj, direct);
	}

	SLAB_BUG_ON(opool.nr!=0);

	tls_freelist_flush(fl, direct);
	SLAB_BUG_ON(fl->nr!=0);
}

static
bool __objpool_free(struct umem_cache_s *s, const void *x, bool direct)
{
	long nr_version = 0;
	struct cache_objpool opool;
	struct cache_objpool *pool = &s->objpool;

again:
	do {
		opool.head = READ_ONCE(pool->head);
		opool.nr_version = READ_ONCE(pool->nr_version);
		/*超过缓存的容量*/
		if (opool.nr+1>s->objpool_cap) {
			objpool_flush(s, direct);
			goto again;
		}
		nr_version = opool.nr_version;
		*(void**)x = opool.head;
		opool.nr+=1;
		opool.verison+=1;
	} while (!cmpxchg_double(&pool->nr_version, &pool->head,
					nr_version, opool.head, opool.nr_version, (void*)x));

	sub_alloc_size(s->size);
	return true;
}

static inline
bool objpool_free(struct umem_cache_s *s, const void *x, bool direct)
{
	if (lockless_cache(s))
		return false;
	return __objpool_free(s, x, direct);
}

static
void *__objpool_alloc(struct umem_cache_s *s)
{
	long nr_version = 0;
	struct cache_objpool opool;
	struct cache_objpool *pool = &s->objpool;

	do {
		opool.head = READ_ONCE(pool->head);
		opool.nr_version = READ_ONCE(pool->nr_version);
		if (!opool.nr||skp_unlikely(!opool.head))
			return NULL;
		nr_version = opool.nr_version;
		opool.nr-=1;
		opool.verison+=1;
		/*消费者获取的 head 中的值可能发生 ABA 问题，所以加版本号*/
	} while (!cmpxchg_double(&pool->nr_version, &pool->head,
				nr_version, opool.head, opool.nr_version, *(void**)opool.head));

	SLAB_BUG_ON(!opool.head);

	add_alloc_size(s->size);

	return opool.head;
}

static inline
void *objpool_alloc(struct umem_cache_s *s)
{
	if (lockless_cache(s))
		return NULL;
	return __objpool_alloc(s);
}

void *__umem_cache_alloc(struct umem_cache_s *s, const char *file, int line)
{
	void *ptr = objpool_alloc(s);
	if (!ptr)
		ptr = cache_alloc(s);
	return leak_insert(ptr, file, line);
}

static inline void cache_free(struct umem_cache_s *s, const void *x)
{
	struct vpage *page;
	page = virt_to_head_page(x);
	SLAB_BUG_ON(!PageSlab(page));
	SLAB_BUG_ON(page->slab != s);
	/*不使用缓存*/
	__slab_free(page, x, NULL, 1, true);
	sub_alloc_size(s->size);
}

void umem_cache_free(struct umem_cache_s *s, const void *x)
{
	leak_remove(x);
	if (!objpool_free(s, x, true))
		cache_free(s, x);
}

void * __umalloc(size_t size, const char *file, int line)
{
	void *ptr;
	struct umem_cache_s *s;
	int idx = get_index(size);

	if (idx > -1) {
		umem_cache_init();
		s = umalloc_caches[idx];
		prefetchw(s);
		ptr = cache_alloc(s);
	} else {
		ptr = __get_free_pages(__GFP_BLK | __GFP_RECLAIM, order_base_2(size)
				- VPAGE_SHIFT);
	}
	return leak_insert(ptr, file, line);
}

void ufree(const void *x)
{
	struct vpage *page;
	struct umem_cache_s *s;

	if (skp_unlikely(IS_ERR_OR_NULL(x)))
		return;

	leak_remove(x);
	page = virt_to_head_page(x);
	BUG_ON(!PageInited(page));
	if (skp_unlikely(!PageSlab(page))) {
		__free_pages(page, block_order(page));
	} else {
		s = page->slab;
		prefetchw(s);
		if (!objpool_free(s, x, false))
			slab_free(s, page, (void *)x);
	}
}

void __ufree(const void *x)
{
	struct vpage *page;
	struct umem_cache_s *s;

	if (skp_unlikely(IS_ERR_OR_NULL(x)))
		return;

	leak_remove(x);
	page = virt_to_head_page(x);
	BUG_ON(!PageInited(page));
	if (skp_unlikely(!PageSlab(page))) {
		___free_pages(page, block_order(page));
	} else {
		s = page->slab;
		prefetchw(s);
		__slab_free(page, x, NULL, 1, true);
		sub_alloc_size(s->size);
	}
}

size_t usize(const void *x)
{
	struct vpage *page;
	if (skp_unlikely(IS_ERR_OR_NULL(x)))
		return 0;
	page = virt_to_head_page(x);
	BUG_ON(!PageInited(page));
	if (skp_unlikely(!PageSlab(page)))
		return VPAGE_SIZE << block_order(page);
	SLAB_BUG_ON(!page->slab);
	return page->slab->size;
}

void * __urealloc(const void *src, size_t size, const char *file, int line)
{
	struct vpage *page;
	void *ptr; size_t l;

	if (skp_unlikely(!src))
		return __umalloc(size, file, line);

	if (skp_unlikely(IS_ERR(src)))
		return NULL;

	if (skp_unlikely(!size)) {
		ufree(src);
		return NULL;
	}

	page = virt_to_head_page(src);
	if (skp_unlikely(!PageSlab(page))) {
		l = VPAGE_SIZE << block_order(page);
	} else {
		SLAB_BUG_ON(!page->slab);
		l = page->slab->size;
	}

	if (l >= size)
		return (void *)src;

	ptr = __umalloc(size, file, line);
	if (skp_likely(ptr)) {
		memcpy(ptr, src, min(size, l));
		ufree(src);
	}
	return ptr;
}

void *__ucalloc(size_t n, size_t size, const char *file, int line)
{
	if (n != 0 && size > ULONG_MAX / n)
		return NULL;
	return __umalloc(n * size, file, line);
}

static void slabtls_reclaim(struct slab_tls *tls)
{
	struct vpage *page;
	struct umem_cache_s *s;

	if (skp_unlikely(!tls))
		return;

	/*此时已不能访问 tls 段了*/
	for (int i = 0; i <= UMALLOC_SHIFT_HIGH; i++) {
		tls_freelist_flush(&tls[i].freelist, true);

		page = tls[i].page;
		if (!page)
			continue;

		s = umalloc_caches[i];
		SLAB_BUG_ON(!s);
		slab_lock(page);
		deactivate_slab(s, page, tls, true);
		SLAB_BUG_ON(tls[i].page);
	}
}

static void slabtls_release(void *ptr)
{
	slabtls_reclaim(ptr);
	cache_free(umalloc_caches[0], ptr);
}

static inline void cache_reclaim(void)
{
	slabtls_reclaim(slab_tls);
}

void umem_cache_reclaim(void)
{
	cache_reclaim();
	pgcache_reclaim();
}

static int compare_partial(void *_, struct list_head *_a, struct list_head *_b)
{
	const struct vpage *a = container_of(_a, struct vpage, lru);
	const struct vpage *b = container_of(_b, struct vpage, lru);
	return a->inuse < b->inuse?-1:(a->inuse==b->inuse?0:1);
}

static void cache_shrink(umem_cache_t *s)
{
	LIST__HEAD(list);
	struct vpage *page, *next;

	objpool_flush(s, true);

	smp_rmb();
	/*装载非 lockfree_cache 时需要加锁分配内存，防止递归死锁*/
	while (!cache_trylock(s)) {
		if (READ_ONCE(s->flags) & __SLAB_SHRINKING)
			return;
	}

	SLAB_BUG_ON(s->flags & __SLAB_SHRINKING);
	smp_wmb();
	s->flags |= __SLAB_SHRINKING;
	smp_mb();

	/*锁住slab描述符后，部分链表中的 page->inuse 只会减少*/
	for_each_page_safe(page, next, cache_partial(s)) {
		/*
		 * 为了排序不必加锁页，这时可能正有对象在释放
		 * 必须以 trylock 方式对页加锁，否则可能死锁，比如
		 * @see deactivate_slab()
		 */
		uint32_t inuse = READ_ONCE(page->inuse);
		if (!inuse && slab_trylock(page)) {
			__remove_partial(s, page);
			slab_unlock(page);
			log_debug("reclaim one slab object : %p, %p[%s]", page, s, s->name);
			discard_slab(s, page, true);
		}
		if (skp_unlikely(!inuse))
			continue;
		move_page_to_list_tail(page, &list);
	}

	if (skp_likely(!list_empty(&list))) {
		if (!list_is_singular(&list))
			list_sort(NULL, &list, compare_partial);
		list_splice_tail_init(&list, cache_partial(s));
	}

	smp_wmb();
	s->flags &= ~__SLAB_SHRINKING;
	cache_unlock(s);
}

void umem_cache_shrink(umem_cache_t *s)
{
	if (skp_unlikely(!s))
		return;

	SLAB_BUG_ON(lockless_cache(s));
	SLAB_BUG_ON(READ_ONCE(slab_state) < SLAB_UP);

	cache_shrink(s);
}

void umem_cache_shrink_all(void)
{
	struct umem_cache_s *s;

	/*防止递归回调此函数*/
	static __thread uint8_t shrinking = 0;

	if (skp_unlikely(shrinking))
		return;

	if (READ_ONCE(slab_state) < SLAB_COMP) {
		pgcache_reclaim();
		return;
	}

	shrinking = 1;
	static_mb();

	log_debug("start shrink all of slab");

	cache_reclaim();

	down_read(&slub_lock);
	for_each_cache_reverse(s) {
		cache_shrink(s);
	}
	up_read(&slub_lock);
	log_debug("finish shrink all of slab");

	pgcache_reclaim();

	shrinking = 0;
}

static inline bool slab_unmergeable(struct umem_cache_s *s)
{
	if (skp_unlikely(s->refcount < 0))
		return true;
	if (s->flags & SLAB_UNMERGEABLE)
		return true;
	return false;
}

static struct umem_cache_s *find_mergeable(uint32_t size, uint32_t align,
		uint32_t flags)
{
	struct umem_cache_s *s;

	size = ALIGN(size, sizeof(void *));
	align = calculate_alignment(flags, align, size);
	size = ALIGN(size, align);
	for_each_cache(s) {
		if (size > s->size)
			continue;
		if (slab_unmergeable(s))
			continue;
		/*
		 * 对齐大于存在slab对象大小，不能合并
		 */
		if ((s->size & ~(align - 1)) != s->size)
			continue;
		/*差值过大不能合并，放置浪费空间*/
		if (s->size - size >= sizeof(void *))
			continue;
		return s;
	}
	return NULL;
}

static void prepare_objpool(struct umem_cache_s *s)
{
#ifndef SLAB_DEBUG
	uint32_t objs = s->objects;
	if (objs>=SLAB_MIN_OBJECTS) {
		objs/=8;
	} else {
		objs/=16;
	}

	objs = clamp(objs, 64U, 512U);
	objs = roundup_pow_of_two(objs);
	s->objpool_cap = objs;
	static_mb();
#endif
}

struct umem_cache_s* umem_cache_create(const char *name, size_t __s, size_t __a,
		uint16_t flags)
{
	struct umem_cache_s *s;
	uint32_t size = (uint32_t)__s;
	uint32_t align = (uint32_t)__a;

	BUG_ON(__s >= (VPAGE_SIZE << (MAX_ORDER - 1)));
	BUG_ON(__a >= (VPAGE_SIZE << (MAX_ORDER - 1)));

	umem_cache_init();

	down_write(&slub_lock);
	/*如果可以，选择一个已存在的cache来满足创建请求*/
	s = find_mergeable(size, align, flags);
	if (s) {
		size = ALIGN(size, sizeof(void *));
		/* 1. 增加引用计数 */
		s->refcount++;
		/* 2. 调整 memset() 时清理的区域 */
		s->inuse = max(s->inuse, size);
		s->objsize = max(s->objsize, size);
	} else {
		/*
		 * 1. 分配一个描述符
		 * 2. 初始化描述符
		 * 3. 加入到全局管理链表
		 */
		s = __create_cache(name, size, align, flags);
		if (skp_unlikely(!s))
			goto fail;
		/*分配无锁缓存*/
		prepare_objpool(s);
		list_add(&s->list, &slab_caches);
	}
	up_write(&slub_lock);
	return s;

fail:
	up_write(&slub_lock);

	if (flags & SLAB_PANIC) {
		log_error("Cannot create slabcache %s\n", name);
		abort();
	}
	return NULL;
}

static int free_partials(struct umem_cache_s *s)
{
	int inuse = 0;
	struct vpage *page, *n;

	cache_lock(s);
	for_each_page_safe(page, n, cache_partial(s)) {
		if (skp_likely(!page->inuse)) {
			__remove_partial(s, page);
			discard_slab(s, page, true);
		} else {
			inuse++;
		}
	}
	cache_unlock(s);
	return inuse;
}

/*冲洗用户自创建的缓存*/
static bool cache_flush(struct umem_cache_s *s)
{
	int rc = 0;

	objpool_flush(s, true);

	do {
		struct vpage *page = get_slab_page(s);
		if (skp_likely(page)) {
			if (skp_unlikely(rc++ > 0)) {
				log_warn("SOMEBODY IS ALLOCATING MEMORY ON THIS CACHE : %p[%s]",
					 s, s->name);
				sched_yield();
			}
			deactivate_slab(s, page, NULL, true);
		}
	} while (skp_unlikely(s->slab_page));

	rc = free_partials(s);
	if (rc || atomic_read(&s->node.nr_slab)) {
		log_warn("SOME OBJECT HAS NOT BEEN FREE ON THIS CACHE %p[%s] YET",
			s, s->name);
		return false;
	}

	return true;
}

bool umem_cache_destroy(struct umem_cache_s *s)
{
	bool rc = true;
	if (skp_unlikely(!s))
		return false;

	down_write(&slub_lock);
	SLAB_BUG_ON(lockless_cache(s) && s->refcount < 2);
	if (!--s->refcount) {
		SLAB_BUG_ON(lockless_cache(s));
		/*不理会这样的内存泄漏，仅给出警告*/
		if (cache_flush(s)) {
			list_del_init(&s->list);
			/*绕过缓存释放*/
			__ufree(s->name);
			cache_free(umalloc_caches[0], s);
		} else {
			rc = false;
			s->refcount++;
		}
	}
	up_write(&slub_lock);
	return rc;
}

char * __uvasprintf(const char *file, int line, const char *fmt, va_list ap)
{
	char *p;
	va_list aq;
	unsigned int first, second;

	va_copy(aq, ap);
	first = vsnprintf(NULL, 0, fmt, aq);
	va_end(aq);

	p = __umalloc(first+1, file, line);
	if (!p)
		return NULL;

	second = vsnprintf(p, first+1, fmt, ap);
	WARN(first != second, "different return values (%u and %u) "
		"from vsnprintf(\"%s\", ...)", first, second, fmt);

	return p;
}

char * __ustrdup(const char *file, int line, const char *ptr)
{
	char *s;
	size_t l, r;
	if (skp_unlikely(!ptr))
		return NULL;
	l = strlen(ptr);
	s = __umalloc(l + 1, file, line);
	if (skp_likely(s)) {
		r = snprintf(s, l + 1, "%s", ptr);
		WARN(r != l, "different return values (%zu and %zu) "
			 "from snprintf(...)", l, r);
	}
	return s;
}

char *__uasprintf(const char *file, int line, const char *fmt, ...)
{
	char *p; va_list ap;
	va_start(ap, fmt);
	p = __uvasprintf(file, line, fmt, ap);
	va_end(ap);
	return p;
}
