//
//  pgalloc.c
//
//  Created by 周凯 on 2019/2/28.
//  Copyright © 2019 zhoukai. All rights reserved.
//

#include <skp/mm/pgalloc.h>
#include <skp/process/wait.h>
#include <skp/process/thread.h>

#include <skp/mm/slab.h>

/*使用在线程私有数据中*/
struct page_tls {
	int count;		/*链表中的页数量 number of pages in the list */
	int low;		/* 低水平位，需要填充 low watermark, refill needed */
	int high;		/* 高水平位，需要清空 high watermark, emptying needed */
	int batch;		/* 批量增加或删除的数量 chunk size for buddy add/remove */
	struct list_head list;	/* the list of pages */
};

//#define BUDDY_DEBUG

#ifdef BUDDY_DEBUG
static bool bad_range(struct zone *zone, struct vpage *page)
{
	pg_data_t *pgdat = zone->pgdata;
	unsigned long pfn = page_to_pfn(page);

	if (skp_unlikely(pfn >= pgdat->start_pfn + VPAGES_PER_NODE))
		return true;
	if (skp_unlikely(pfn < pgdat->start_pfn))
		return true;
	if (skp_unlikely(zone != page_zone(page)))
		return true;
	return false;
}
static inline void free_pages_check(struct vpage *page)
{
	if(skp_unlikely(PageBuddy(page))) {
		log_error("double free : virt %p, page %p",
			page_to_virt(page), page);
		BUG();
	}
	if (skp_unlikely(page_count(page))) {
		log_error("non-zero refs : virt %p, page %p",
			page_to_virt(page), page);
		BUG();
	}
	WARN_ON(page->flags &(1UL << PG_locked | 1UL << PG_slab));
	page->flags &= ~(1UL << PG_locked | 1UL << PG_slab);
}
# define BUDDY_BUG_ON(x) BUG_ON((x))
#else
# define bad_range(z, p) (false)
# define free_pages_check(p)
# define BUDDY_BUG_ON(x)
#endif

static __thread struct page_tls *page_tls = NULL;

static inline int32_t page_order(struct vpage *page)
{
	return PageBuddy(page) ? page->order : -1;
}

static inline bool page_is_buddy(struct vpage *page, int order)
{
	if (page_order(page) != order)
		return false;
	BUDDY_BUG_ON(PageReserved(page));
	return true;
}

static inline void set_page_order(struct vpage *page, int order)
{
	page->order = order;
	__SetPageBuddy(page);
}

static inline void rmv_page_order(struct vpage *page)
{
	__ClearPageBuddy(page);
	page->order = -1;
}

static inline void prep_new_page(struct vpage *page, int order)
{
	BUDDY_BUG_ON(!(page->flags & (1UL << PG_inited)));
	BUDDY_BUG_ON(page->flags &(
		1UL << PG_locked | 1UL << PG_slab | 1UL << PG_compound));
	page->flags &= ~(
		1UL << PG_locked | 1UL << PG_slab | 1UL << PG_compound);
	page->data = NULL;
	uref_init(&page->count);
}

static __always_inline void prep_compound_page(struct vpage *page, int order,
		int gfp_flags)
{
	int nr_pages = 1 << order;

	if (!(order && (gfp_flags & __GFP_COMP)))
		return;

	/*第二个页的 order 存放组合页的 order*/
	page[1].order = order;
	/*每个页的 head 指向领头页*/
	for (int i = 0; i < nr_pages; i++) {
		struct vpage *p = page + i;
		__SetPageCompound(p);
		p->head = page;
	}
}

static __always_inline void destroy_compound_page(struct vpage *page, int order)
{
	int i;
	int nr_pages = 1 << order;

	if (!order || !PageCompound(page))
		return;

	if (skp_unlikely(page[1].order != order)) {
		log_error("expect order of compound pages %d,"
			" but get %d", order, page[1].order);
		BUG();
	}

	page[1].order = -1;
	for (i = 0; i < nr_pages; i++) {
		struct vpage *p = page + i;

		if (skp_unlikely(compound_head(p) != page)) {
			log_error("expect page is a part of compound pages");
			BUG();
		}
		__ClearPageCompound(p);
		p->head = NULL;
	}
}

static inline void prep_block_page(struct vpage *page, int order, int gfp_flags)
{
	if (!(order && (gfp_flags & __GFP_BLK)))
		return;
	page[1].order = order;
	__SetPageBlock(page);
}

static __always_inline void prep_zero_page(struct vpage *page, int order,
		int gfp_flags)
{
	if (!(gfp_flags & __GFP_ZERO))
		return;
	for(int i = 0; i < (1 << order); i++)
		memset(page_to_virt(page + i), 0, VPAGE_SIZE);
}

static inline void destroy_block_page(struct vpage *page, int order)
{
	if (!order || !PageBlock(page))
		return;
	__ClearPageBlock(page);
	page[1].order = -1;
}

static __always_inline
void __free_pages_bulk(struct vpage *page, struct zone *zone, int order)
{
	unsigned long page_idx, buddy_idx;
	struct free_area *area = &zone->free_area[order];
	struct vpage *buddy, *base = mem_map(page_to_nid(page));

	/*被释放页在所在node中的索引*/
	page_idx = page - base;
	/*页在所在的zone中的索引和连续性要一致*/
	BUDDY_BUG_ON(!base);
	BUDDY_BUG_ON(PageReserved(page));
	BUDDY_BUG_ON(bad_range(zone, page));
	BUDDY_BUG_ON(page_idx & (order_size - 1));

	if (skp_unlikely(!zone->free_pages))
		set_bit(page_to_nid(page), node_map.has_free);

	zone->free_pages += (1U << order);
	while (order < MAX_ORDER-1) {
		buddy_idx = (page_idx ^ (1UL << order));
		buddy = base + buddy_idx;
		if (bad_range(zone, buddy))
			break;
		if (!page_is_buddy(buddy, order))
			break;

		/* Move the buddy up one level. */
		area->nr_free--;
		rmv_page_order(buddy);
		del_page_from_list(buddy);

		order++;
		area++;
		page_idx &= buddy_idx;
	}

	area->nr_free++;
	set_page_order(base + page_idx, order);
	add_page_to_list(base + page_idx, &area->free_list);

	if (skp_likely(order < MAX_ORDER-1))
		return;

	/*
	 * 回收一些连续巨页，必须保留一个，否则会死循环
	 * @see __node_supply_memory()
	 */
	while (area->nr_free > 1) {
		BUDDY_BUG_ON(list_empty(&area->free_list));
		area->nr_free--;
		page = __pop_page_on_list(&area->free_list);
		BUDDY_BUG_ON(page_to_virt(page) == base);
		zone->free_pages -= 1U << order;
		if (WARN_ON(!zone->free_pages))
			clear_bit(page_to_nid(page), node_map.has_free);
		/*
		 *可以解锁释放
		 * 1. 实质上页仍在伙伴系统中（页标志完整）
		 * 2. 该段虚地址仍被内核mmap管理，不可能和 __node_supply_memory() 冲突
		 */
		zone_unlock(zone);
		node_reclaim_memory(page, MAX_ORDER-1);
		zone_lock(zone);
	}

}

static int free_pages_bulk(int count, struct list_head *list, int order)
{
	int rc = 0;
	struct zone *zone;
	struct vpage *page;
	while (!list_empty(list) && count--) {
		/*从尾部开始释放*/
		page = last_page_on_list(list);
		/* list 必须是局部的链表 */
		del_page_from_list(page);

		/*解除页的标志和状态等*/
		destroy_block_page(page, order);
		destroy_compound_page(page, order);

		zone = page_zone(page);
		zone_lock(zone);
		__free_pages_bulk(page, zone, order);
		zone_unlock(zone);
		rc++;
	}
	WARN_ON(count > 0);
	return rc;
}

static void __pgtls_reclaim(void *ptr)
{
	struct page_tls *tls = ptr;
	if (skp_unlikely(!tls))
		return;
	tls->count -= free_pages_bulk(tls->count, &tls->list, 0);
}

static void pgtls_reclaim(void *ptr)
{
	__pgtls_reclaim(ptr);
	__ufree(ptr);
}

static __always_inline struct page_tls *pgtls_acquire(void)
{
	struct page_tls *tls = page_tls;

	if (skp_unlikely(READ_ONCE(slab_state) < SLAB_COMP))
		return NULL;

	if (skp_likely(tls))
		return skp_likely(tls!=(void*)0x01UL) ? tls : NULL;

	page_tls = (void*)0x01UL;
	/*此处可能引起递归*/
	tls = umalloc(sizeof(*tls));
	if (skp_unlikely(!tls))
		return NULL;
	BUG_ON(page_tls != (void*)0x01UL);

	tls->count = 0;
	tls->low = CONFIG_PAGESET_BATCH * 2;
	tls->high = CONFIG_PAGESET_BATCH * 3,
	tls->batch = CONFIG_PAGESET_BATCH,
	INIT_LIST_HEAD(&tls->list);

	/*必须先设置，防止再次递归*/
	page_tls = tls;

	tlsclnr_register(pgtls_reclaim, tls);

	return tls;
}

void pgcache_reclaim(void)
{
	if (!READ_ONCE(node_up))
		return;
	__pgtls_reclaim(page_tls);
}

void free_hot_page(struct vpage *page)
{
	struct page_tls *tls = pgtls_acquire();

	if (skp_unlikely(!tls)) {
		__free_pages_ok(page, 0);
		return;
	}

	free_pages_check(page);
	if (skp_unlikely(tls->count >= tls->high))/*过多则归还给伙伴系统*/
		tls->count -= free_pages_bulk(tls->batch, &tls->list, 0);
	/*归还到头部，可以更好的利用 cache*/
	add_page_to_list(page, &tls->list);
	tls->count++;
}

void __free_pages_ok(struct vpage *page, int order)
{
	LIST__HEAD(list);

	BUG_ON(order >= MAX_ORDER || order < 0);
	for (int i = 0; i < (1 << order); i++)
		free_pages_check(page + i);
	/*插入到临时链表，以备释放*/
	add_page_to_list_tail(page, &list);
	free_pages_bulk(1, &list, order);
}

void __free_pages(struct vpage *page, int order)
{
	if (put_page_testzero(page)) {
		if (order == 0) {
			/*单个页直接放入per-cpu的热页缓存*/
			free_hot_page(page);
		} else {
			__free_pages_ok(page, order);
		}
		return;
	}
	BUG_ON(page_count(page) < 0);
}

void ___free_pages(struct vpage *page, int order)
{
	if (put_page_testzero(page)) {
		__free_pages_ok(page, order);
		return;
	}
	BUG_ON(page_count(page) < 0);
}

void __put_page(struct vpage* page)
{
	if (PageCompound(page)) {
		BUDDY_BUG_ON(page != compound_head(page));
		__free_pages_ok(page, page[1].order);
	} else if (PageBlock(page)) {
		BUDDY_BUG_ON(page[1].order < 1);
		__free_pages_ok(page, page[1].order);
	} else {
		free_hot_page(page);
	}
}

static __always_inline
struct vpage *expand(struct zone *zone, struct vpage *page, int low, int high,
		struct free_area *area)
{
	unsigned long size = 1UL << high;
	BUDDY_BUG_ON(bad_range(zone, page));
	while (high > low) {/*如果high>low，连续的页框可以被分裂*/
		area--;/*递减阶和对应的空闲链表数组指针*/
		high--;/*阶数递减*/
		size >>= 1;/* 每递减一次，连续的页框数减半，（二分）
					* 前半部分用于下次分裂，后半部分插入紧挨的低阶空闲链表
					* 此处计算相应的后半部分的页偏移
					*/
		BUDDY_BUG_ON(bad_range(zone, &page[size]));
		area->nr_free++;
		/*设置每组连续页框的头框的private字段为对应的阶值*/
		set_page_order(&page[size], high);
		/*后半部分插入伙伴系统中*/
		add_page_to_list_tail(&page[size], &area->free_list);
	}
	return page;
}

static struct vpage *__rmqueue(struct zone *zone, int order)
{
	struct vpage *page;
	struct free_area *area;
	/*
	 * 从分配阶链表向高级阶链表请求连续的页框
	 */
	for (int k = order; k < MAX_ORDER; ++k) {
		area = &zone->free_area[k];
		if (!area->nr_free)
			continue;
		BUDDY_BUG_ON(list_empty(&area->free_list));
		page = __pop_page_on_list(&area->free_list);
		/*移除private中存的order值*/
		rmv_page_order(page);
		area->nr_free--;
		zone->free_pages -= 1UL << order;
		page = expand(zone, page, order, k, area);
		if (skp_unlikely(!zone->free_pages))
			clear_bit(page_to_nid(page), node_map.has_free);
		return page;
	}

	return NULL;
}

static __always_inline int rmqueue_bulk(struct zone *zone, int order, int count,
		struct list_head *list)
{
	int nr = 0;
	zone_lock(zone);
	for (int i = 0; i < count; ++i) {
		struct vpage *page = __rmqueue(zone, order);
		if (skp_unlikely(!page))
			break;
		nr++;
		add_page_to_list_tail(page, list);
	}
	zone_unlock(zone);
	return nr;
}

static struct vpage *buffered_rmqueue(struct zone *zone, int order, int flags)
{
	struct vpage *page;
	struct page_tls *tls;

	if (order != 0) {
slow:
		/*非单页分配，或者slab分配器尚未完成初始化*/
		zone_lock(zone);
		page = __rmqueue(zone, order);
		zone_unlock(zone);
		if (skp_unlikely(!page))
			return NULL;
		goto prep;
	}

	/*只需要一页时，从缓存在per-thread中获取*/
	tls = pgtls_acquire();
	if (skp_unlikely(!tls))
		goto slow;

	/*过低，则从buddy中分配一批单个的页框，并插入pcp->list中*/
	if (skp_unlikely(tls->count <= tls->low))
		tls->count += rmqueue_bulk(zone, 0, tls->batch, &tls->list);

	/*弹出一个用于本次分配*/
	page = pop_page_on_list(&tls->list);
	if (skp_unlikely(!page))
		return NULL;
	BUDDY_BUG_ON(tls->count < 1);
	tls->count--;

prep:
	/*检查flags，mapping字段，并设置 _count=0 字段*/
	prep_new_page(page, order);
	/*通过临时映射清零对应的页框数据*/
	prep_zero_page(page, order, flags);
	/*块页*/
	prep_block_page(page, order, flags);
	/*复合页，则设置flags和private字段*/
	prep_compound_page(page, order, flags);

	return page;
}

struct vpage * __alloc_pages(int gfp_flags, int order)
{
	/*每次分配顶多回收一次*/
	int rc = 0;
	unsigned long nid;
	bool shrink = false;
	struct vpage *page = NULL;
	static __thread uint64_t last_shrink = 0;

	if (WARN_ON(order >= MAX_ORDER))
		return NULL;

	setup_memory();

#ifndef __x86_64__
	if (gfp_flags & __GFP_WAIT)
		WARN_ON(in_atomic());
#endif

	/*如果是单页，首先查看0号节点 是否满足*/
	if (!order) {
		page = buffered_rmqueue(NODE_ZONE(0), order, gfp_flags);
		if (skp_likely(page))
			return page;
	}

	do {
		for_each_free_node(nid) {
			/*分配巨页时，提前查看该节点是否满足分配*/
			if (skp_unlikely(order > MAX_ORDER/2) &&
					!node_has_freepg((int)nid, order))
				continue;
			page = buffered_rmqueue(NODE_ZONE(nid), order, gfp_flags);
			if (skp_likely(page))
				return page;
		}

		/*TODO:回收算法*/
		if ((gfp_flags & __GFP_RECLAIM) && !shrink) {
			uint64_t now = similar_abstime(0, 0);
			shrink = true;
			/*大约 10 毫秒收缩一次*/
			if ((now - last_shrink) > (10UL << 20)) {
				last_shrink = now;
				umem_cache_shrink_all();
				continue;
			}
		}

		/*需要补充内存*/
		rc = node_supply_memory(order);
	} while (skp_likely(rc > -1));

#if BITS_PER_LONG == 32
	/*todo : 阻塞等待？*/
#endif

	return page;
}

static int sync_page(wait_queue_t *wait)
{
	wait_on(wait);
	return 0;
}

static inline void wake_up_page(struct vpage *page, int bit_nr)
{
	wake_up_bit(&page->flags, bit_nr);
}

void wait_on_page_bit(struct vpage *page, int bit_nr)
{
	wait_on_bit(&page->flags, bit_nr, sync_page);
}

void __lock_page(struct vpage *page)
{
	out_of_line_wait_on_bit_lock(&page->flags, PG_locked, sync_page);
}

void unlock_page(struct vpage *page)
{
	smp_wmb();
	if (!TestClearPageLocked(page))
		BUG();
	wake_up_page(page, PG_locked);
}
