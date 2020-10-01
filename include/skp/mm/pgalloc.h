//
//  pgalloc.h
//
//  Created by 周凯 on 2019/2/28.
//  Copyright © 2019 zhoukai. All rights reserved.
//

#ifndef __US_PGALLOC_H__
#define __US_PGALLOC_H__

#include "../utils/uref.h"
#include "../adt/list.h"
#include "mmzone.h"

__BEGIN_DECLS

#define NODEZONE_SHIFT (BITS_PER_LONG - MAX_NODES_SHIFT)

#ifndef CONFIG_PAGESET_BATCH
# define CONFIG_PAGESET_BATCH 4
#endif

enum {
	__GFP_WAIT = 0x01,/**<允许当前进程阻塞*/
	__GFP_COMP = 0x02,/**<属于扩展页的页框*/
	__GFP_BLK = 0x04, /**<分配的连续页被看做块*/
	__GFP_ZERO = 0x08,/**<清零数据页*/
	__GFP_RECLAIM = 0x10, /**<启用回收*/
};

enum {
	PG_locked,
	PG_slab,
	PG_compound,
	PG_buddy,
	PG_active,
	PG_block, /**< 分配的续页作为块整体，order 存储在第二页中*/
	PG_inited, /**<全局初始化标志，对应的虚地址只能初始化一次*/
	PG_reserved, /**< 保留页框*/
	PG_MAX_FLAG,
};

struct umem_cache_s;
/**
 * 虚拟页描述符
 */
struct vpage {
	unsigned long flags; /* Atomic flags, some possibly*/
	struct uref count;
	int32_t order;
	struct list_head lru;
	union {
		void *data;
		struct vpage *head;
	};
	struct {	/* SLUB uses */
		uint32_t inuse; /**< 已分配对象的计数，也是无锁链表管理的对象数量
			* 是lockless_freelist 链表中缓存对象的计数*/
		void *freelist;		/**< 第一个可用对象的指针*/
		void **lockless_freelist; /**< 单个对象的 meta 的指针，
			* 解引用后得到下一个可用位置，栈方式来操作对象链表
			* 用于 per-CPU 无锁分配
			* 原理是将 freelist 后所接的 slab 对象空间 全部看成
			* void * 组成的数组，然后 使用 offset 给出的到 meta 区
			* 的数组个数，然后解引用后 就获取了meta区中下个可用对象
			* 的起始地址，依旧看成 void* 的数组处理，以此类推。 */
		struct umem_cache_s *slab;	/**< 指向所属的slab对象 SLUB: Pointer to slab */
	};
};

////////////////////////////////////////////////////////////////////////////////
/*每个node保留的内存大小和阶数*/
#define RESERVED_MULT_PER_NODE												\
	roundup_pow_of_two(DIV_ROUND_UP(VPAGES_PER_NODE*sizeof(struct vpage),	\
		VPAGE_SIZE))

#define RESERVED_SIZE_PER_NODE	(RESERVED_MULT_PER_NODE << VPAGE_SHIFT)
#define RESERVED_ORDER_PER_NODE	ilog2(RESERVED_SIZE_PER_NODE/VPAGE_SIZE)

#define pfn_valid(pfn)          ((pfn) < MAX_NR_VPAGES)
#define virt_addr_valid(addr)	pfn_valid(((uintptr_t)(addr)) >> VPAGE_SHIFT)

/**将node的ID存放在高位8位*/
static inline void set_page_node(struct vpage *page, int nid)
{
	page->flags &= ~(ULONG_MAX << NODEZONE_SHIFT);
	page->flags |= (unsigned long)nid << NODEZONE_SHIFT;
}

static inline int page_to_nid(const struct vpage *page)
{
	return (int)(page->flags >> NODEZONE_SHIFT);
}

static inline struct vpage *pfn_to_page(unsigned long pfn)
{
	int nid  = pfn_to_nid(pfn);
	return &mem_map(nid)[node_locnr(pfn,nid)];
}

static inline unsigned long page_to_pfn(const struct vpage *page)
{
	int nid = page_to_nid(page);
	return (unsigned long)(page - mem_map(nid)) + start_pfn(nid);
}

static inline struct vpage *virt_to_page(const void *ptr)
{
	return pfn_to_page(virt_to_pfn(ptr));
}

static inline void *page_to_virt(const struct vpage *page)
{
	return (void*)(page_to_pfn(page) << VPAGE_SHIFT);
}

static inline struct zone *page_zone(struct vpage *page)
{
	return &NODE_DATA(page_to_nid(page))->node_zones[0];
}

////////////////////////////////////////////////////////////////////////////////

/**清理线程页分配器的私有数据*/
extern void pgcache_reclaim(void);
/**分配 2^order 虚拟页，返回连续页的头页*/
extern struct vpage * __alloc_pages(int gfp_flags, int order);

/*一般不使用 __free 系列的函数，而是用 put_page 系列函数*/

/**直接释放 2^order 虚拟到伙伴系统*/
extern void __free_pages_ok(struct vpage *page, int order);
/**释放 单页 到per-thread缓存中，缓存满了才会被批量的返回到伙伴系统*/
extern void free_hot_page(struct vpage *page);

/**会查看引用计数是否为0*/
extern void __free_pages(struct vpage *page, int order);
/*忽略TLS缓存，查看引用计数，释放到伙伴系统*/
extern void ___free_pages(struct vpage *page, int order);
////////////////////////////////////////////////////////////////////////////////
/*各种辅助函数*/

#define alloc_pages(flags, order) __alloc_pages((flags)|__GFP_RECLAIM, (order))
#define alloc_page(flags) alloc_pages((flags), 0)

/*以下辅助函数直接返回虚地址*/
static inline void* __get_free_pages(int gfp_mask, int order)
{
	struct vpage * page;
	page = __alloc_pages(gfp_mask, order);
	return skp_likely(page) ? page_to_virt(page) : NULL;
}

static inline void* get_zeroed_page(int gfp_mask)
{
	struct vpage * page;
	page = __alloc_pages(gfp_mask | __GFP_ZERO, 0);
	return skp_likely(page) ? page_to_virt(page) : NULL;
}

#define get_free_page(gfp_flags) __get_free_pages((gfp_flags), 0)

/**释放虚地址关联的 2^order 个连续页*/
static inline void free_pages(void *addr, int order)
{
	if (skp_likely(addr)) {
		BUG_ON(!virt_addr_valid(addr));
		__free_pages(virt_to_page(addr), order);
	}
}

#define free_page(addr) free_pages((addr), 0)

////////////////////////////////////////////////////////////////////////////////

#define __defpg_func(name, field) \
static inline bool Page##name(struct vpage *page) \
{ return test_bit(field, &page->flags); } \
static inline void __SetPage##name(struct vpage *page) \
{ __set_bit(field, &page->flags); } \
static inline void __ClearPage##name(struct vpage *page) \
{ __clear_bit(field, &page->flags); } \
static inline void SetPage##name(struct vpage *page) \
{ set_bit(field, &page->flags); } \
static inline void ClearPage##name(struct vpage *page) \
{ clear_bit(field, &page->flags); } \
static inline bool TestSetPage##name(struct vpage *page) \
{ return test_and_set_bit(field, &page->flags); } \
static inline bool TestClearPage##name(struct vpage *page) \
{ return test_and_clear_bit(field, &page->flags); }

__defpg_func(Buddy, PG_buddy)
__defpg_func(Compound, PG_compound)
__defpg_func(Slab, PG_slab)
__defpg_func(Locked, PG_locked)
__defpg_func(Active, PG_active)
__defpg_func(Block, PG_block)
__defpg_func(Inited, PG_inited)
__defpg_func(Reserved, PG_reserved)

////////////////////////////////////////////////////////////////////////////////
/*
 * 一些页与链表的辅助函数
 */
static inline bool page_on_list(const struct vpage *page)
{
	return !list_empty(&page->lru);
}

static inline struct vpage *first_page_on_list(struct list_head *head)
{
	return list_first_entry_or_null(head, struct vpage, lru);
}

static inline struct vpage *last_page_on_list(struct list_head *head)
{
	return list_last_entry(head, struct vpage, lru);
}

static inline struct vpage *__first_page_on_list(struct list_head *head)
{
	return list_first_entry(head, struct vpage, lru);
}

static inline void add_page_to_list_tail(struct vpage *page,
		struct list_head *head)
{
	list_add_tail(&page->lru, head);
}

static inline void add_page_to_list(struct vpage *page, struct list_head *head)
{
	list_add(&page->lru, head);
}

static inline void del_page_from_list(struct vpage *page)
{
	list_del_init(&page->lru);
}

static inline struct vpage *__pop_page_on_list(struct list_head *head)
{
	struct vpage * page = __first_page_on_list(head);
	list_del_init(&page->lru);
	return page;
}

static inline struct vpage *pop_page_on_list(struct list_head *head)
{
	struct vpage * page = first_page_on_list(head);
	if (page)
		list_del_init(&page->lru);
	return page;
}

static inline void move_page_to_list(struct vpage *page, struct list_head *head)
{
	list_move(&page->lru, head);
}

static inline void move_page_to_list_tail(struct vpage *page,
		struct list_head *head)
{
	list_move_tail(&page->lru, head);
}

#define for_each_page(p, l)				\
	list_for_each_entry((p), l, lru)

#define for_each_page_safe(p, n, l)		\
	list_for_each_entry_safe((p), (n), l, lru)

////////////////////////////////////////////////////////////////////////////////
static inline int block_order(struct vpage *page)
{
	return PageBlock(page) ? page[1].order : 0;
}

static inline int compound_order(struct vpage *page)
{
	return PageCompound(page) ? page->head[1].order : 0;
}

static inline struct vpage * compound_head(struct vpage *page)
{
	return PageCompound(page) ? page->head : page;
}

static inline struct vpage * virt_to_head_page(const void *ptr)
{
	return compound_head(virt_to_page(ptr));
}

////////////////////////////////////////////////////////////////////////////////
/*
 * 自旋试的锁
 */
static inline void page_lock(struct vpage *page)
{
	bit_spin_lock(PG_locked, &page->flags);
}

static inline void page_unlock(struct vpage *page)
{
	bit_spin_unlock(PG_locked, &page->flags);
}

static inline bool page_trylock(struct vpage *page)
{
	return bit_spin_trylock(PG_locked, &page->flags);
}

////////////////////////////////////////////////////////////////////////////////
/*
 * 阻塞试的锁
 */
extern void __lock_page(struct vpage *page);
extern void unlock_page(struct vpage *page);
#define trylock_page(p) page_trylock((p))

static inline void lock_page(struct vpage *page)
{
	if (TestSetPageLocked(page))
		__lock_page(page);
}

/*
 * This is exported only for wait_on_page_locked/wait_on_page_writeback.
 * Never use this directly!
 */
extern void wait_on_page_bit(struct vpage *page, int bit_nr);

////////////////////////////////////////////////////////////////////////////////

static inline bool put_page_testzero(struct vpage *page)
{
	return __uref_put(&page->count);
}

static inline bool get_page_unless_zero(struct vpage *page)
{
	return uref_get_unless_zero(&page->count);
}

////////////////////////////////////////////////////////////////////////////////
/*可以处理组合页*/

static inline int page_count(struct vpage *page)
{
	return uref_read(&compound_head(page)->count);
}

static inline bool page_testone(struct vpage *page)
{
	return page_count(page) == 1;
}

static inline void ___get_page(struct vpage *page)
{
	uref_get(&page->count);
}

static inline void __get_page(struct vpage *page)
{
	uref_get(&compound_head(page)->count);
}

static inline struct vpage *get_page(struct vpage *page)
{
	return get_page_unless_zero(compound_head(page)) ? page : NULL;
}

/**不会理会引用计数是否已为0，直接释放页*/
extern void __put_page(struct vpage*);
static inline void put_page(struct vpage *page)
{
	if (skp_likely(page)) {
		page = compound_head(page);
		if (put_page_testzero(page))
			__put_page(page);
	}
}

__END_DECLS

#endif /* __US_PGALLOC_H__ */
