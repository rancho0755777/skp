//
//  mmzone.c
//
//  Created by 周凯 on 2019/2/27.
//  Copyright © 2019 zhoukai. All rights reserved.
//

#include <skp/mm/mmzone.h>
#include <skp/mm/pgalloc.h>

//#define CONFIG_BUDDY_SHAREDMMAP

//#define ZONE_DEBUG

#ifndef CONFIG_BUDDY_TEMPFILE
# define CONFIG_BUDDY_TEMPFILE "/tmp/buddy.alloctor.XXXXXX"
#endif

#ifdef ZONE_DEBUG
# define ZONE_BUG_ON(x) BUG_ON((x))
#else
# define ZONE_BUG_ON(x)
#endif

bool node_up = false;
struct pglist_map node_map __cacheline_aligned;
struct pglist_data node_data[MAX_NUMNODES] __cacheline_aligned;
/**下标对应页帧号范围（ [idx ~ (idx + 1)] * PAGES_PER_ELEMENT)，存储该页帧所属的节点号*/
uint8_t physnode_map[MAX_ELEMENTS] __cacheline_aligned = {
	[0 ... (MAX_ELEMENTS - 1)] = -1
};

struct node_config {
	unsigned long start_pfn;
	unsigned long end_pfn;
};

#include <sys/mman.h>

#ifndef MAP_NORESERVE
# define MAP_NORESERVE 0
#endif

#define PROT_FLAGS (PROT_READ|PROT_WRITE)

#ifdef CONFIG_BUDDY_FILEMMAP
static inline void *os_mmap(void *fixed, size_t size)
{
	int fd;
	void *addr;
	char path[128];

#ifdef CONFIG_BUDDY_SHAREDMMAP
# define MAP_FLAGS (MAP_FILE | MAP_SHARED)
#else
# define MAP_FLAGS (MAP_FILE | MAP_PRIVATE | MAP_NORESERVE)
#endif

	snprintf(path, sizeof(path), "%s", CONFIG_BUDDY_TEMPFILE);
	fd = mkstemp(path);
	if (skp_unlikely(fd < 0)) {
		log_error("create tempfile failed : %s", strerror_local());
		return NULL;
	}
	truncate(path, size);
	remove(path);
	/*TODO:匿名文件 + MAP_SHARED，解决OOM killer 问题？
	 *TODO:启动成功后，使用 map_fixed 解决多余一倍内存映射再
	 *解映射来取对齐段的方式
	 */
	addr = mmap(0, size, PROT_FLAGS, MAP_FLAGS, fd, 0);
	close(fd);

	return addr == MAP_FAILED ? NULL : addr;
}
#else
static inline void *os_mmap(void *fixed, size_t size)
{
#ifdef CONFIG_BUDDY_SHAREDMMAP
# define MAP_FLAGS (MAP_ANON | MAP_SHARED)
#else
# define MAP_FLAGS (MAP_ANON | MAP_PRIVATE | MAP_NORESERVE)
#endif
	void *addr = mmap(fixed, size, PROT_FLAGS, MAP_FLAGS, -1, 0);
	return skp_unlikely(addr == MAP_FAILED) ? NULL : addr;
}
#endif

static inline int os_munmap(void *ptr, size_t size)
{
	return munmap(ptr, size);
}

/*获取内存节点分布图*/
static void init_node_config(struct node_config node_cfg[])
{
	uint32_t nid;
	/* 获取内存节点分布图 */
	for_each_node(nid) {
		/*
		 * 计算每个节点 页框的起止（双端闭包）
		 * 最终形成如下的分布
		 * 假设每个节点容纳的页框为X
		 * [0 ~ (X-1)][X ~ (2X-1)][2X ~ (3X-1)][3X ~ (4X-1)]
		 * 这些页框的起止是为了初始化 页框号与节点号的 映射表
		 * 以及页框地址到页框号的转换
		 */
		node_cfg[nid].start_pfn = nid * VPAGES_PER_NODE;
		node_cfg[nid].end_pfn = (nid + 1) * VPAGES_PER_NODE - 1;

		log_debug("node: %u, start_pfn: %lu, end_pfn: %lu",
			nid, node_cfg[nid].start_pfn, node_cfg[nid].end_pfn);

		log_debug("\treserving %zu Kbytes for lmem_map of node %u",
				(VPAGES_PER_NODE * sizeof(struct vpage))>> 10, nid);
		log_debug("\tsetting physnode_map array to node %u for pfns: ", nid);
		/*
		 * 初始化pfn到node的映射
		 * 1. 当node的当前配置数量与默认的最大数量一致时，形成一一映射。
		 * 2. 当node的当前配置数量小于默认的最大数量时（但会是倍数），如下
		 *
		 * |<----n0----->|<----n1----->|
		 * |<-p0->|<-p1->|<-p2->|<-p3->|
		 * | v=0  | v=0  | v=1  | v=1  |
		 * 也就是说 每个 node 包含的页框 将按 VPAGES_PER_ELEMENT 的步长
		 * 在分割掉。
		 */
		for (unsigned long pfn = node_cfg[nid].start_pfn;
				pfn < node_cfg[nid].end_pfn; pfn += VPAGES_PER_ELEMENT) {
			physnode_map[pfn >> VPAGES_PER_ELEMENT_SHIFT] = nid;
			log_debug("\t\tnode map : %lu ", pfn);
		}
	}
}

/**分配一个伙伴系统所需的最大内存块，用于补充某个节点中的伙伴系统的内存*/
static void *alloc_memblock(size_t size)
{
	uintptr_t addr, start;
	static unsigned long total_vm = 0;

	size = ALIGN(size, VPAGE_SIZE);
	addr = (uintptr_t)os_mmap(0, size);
	if (skp_unlikely(!addr)) {
		log_error("mmap to [%zu]MB failed : %s[%d], total vm : %lu\n",
			size >> 20, strerror_local(), errno, total_vm);
		return NULL;
	}

	if (skp_likely(IS_ALIGNED(addr, size))) {
		log_debug("alloc one buddy block : buddy [%p, %p)",
			addr, (void*)((uintptr_t)addr + BUDDY_BLKSIZE));
		return (void*)addr;
	}

/*slow path*/
	BUG_ON(os_munmap((void*)addr, size));
	addr = (uintptr_t)os_mmap(0, size << 1);
	if (skp_unlikely(!addr)) {
		log_error("mmap to [%zu]MB failed : %s[%d], total vm : %lu\n",
			size >> 20, strerror_local(), errno, total_vm);
		return NULL;
	}

	/*取对齐的部分*/
	start = ALIGN(addr, size);
	log_debug("alloc one buddy block : src %p, buddy [%p, %p)",
		addr, (void*)start, (void*)(start + BUDDY_BLKSIZE));
	/*解除不需要部分的内存映射*/
	if (start != addr)
		BUG_ON(os_munmap((void*)addr, start - addr));
	BUG_ON(os_munmap((void*)(start + size), addr + size - start));

	/*todo:buddy中总的页数统计*/
	total_vm += size;
	return (void*)start;
}

/* 初始化node中的虚地址对应的页描述符
 * Todo : 不能多次初始化
 */
static inline void init_vpage(struct vpage *page, int nid, bool reserve)
{
	if (PageInited(page))
		return;

	memset(page, 0, sizeof(*page));

	page->order = -1;
	__SetPageInited(page);
	if (reserve)
		__SetPageReserved(page);

	uref_set(&page->count, 0);
	set_page_node(page, nid);
	INIT_LIST_HEAD(&page->lru);
}

static void init_node_memmap(int nid, void *addr, size_t size)
{
	struct vpage *page;
	uintptr_t start = (uintptr_t)addr,
		end = start + size;

	ZONE_BUG_ON(!ptr);
	ZONE_BUG_ON(size > BUDDY_BLKSIZE);
	ZONE_BUG_ON(!IS_ALIGNED(size, VPAGE_SIZE));
	ZONE_BUG_ON(!test_bit(nid, node_map.has_up));

	page = virt_to_page(addr);
	while (start < end) {
		init_vpage(page++, nid, false);
		start += VPAGE_SIZE;
	}
}

/*
 * 启动页管理节点
 * 将会从映射页的头部截取一段作为页描述符的内存
 */
static void startup_node(int nid, void **pptr)
{
	pg_data_t *pgdata;
	struct vpage *page;
	void *addr = *pptr;
	uintptr_t pfn, start = (uintptr_t)addr,
		end = start + RESERVED_SIZE_PER_NODE;

	pgdata = NODE_DATA(nid);
	BUG_ON(pgdata->mem_map);
	BUG_ON(!IS_ALIGNED(start, VPAGE_SIZE));

	log_info("start up buddy system of [%u] node, reserve addr [%p ~ %p) "
		"for virtual page descriptor", nid, (void*)start, (void*)end);

	*pptr = (void*)end;
	/*初始化保留页框描述符*/
	pfn = virt_to_pfn(addr);
	/*注意，虚拟页描述符内存对应的自身描述符不一定是在保留内存头部*/
	page = &((struct vpage*)start)[node_locnr(pfn, nid)];
	while (start < end) {
		init_vpage(page++, nid, true);
		start += VPAGE_SIZE;
	}

	smp_mb();
	WRITE_ONCE(pgdata->mem_map, addr);
	set_bit(nid, node_map.has_up);

	ZONE_BUG_ON(start != end);
	ZONE_BUG_ON(virt_to_page((void*)end) != page);
}

static void __node_supply_memory(void *addr, size_t size)
{
	int nid;
	bool has_up;
	struct zone *zone;
	struct vpage *page;
	uintptr_t end = (uintptr_t)addr + size;

	nid = pfn_to_nid(virt_to_pfn(addr));
	has_up = test_bit(nid, node_map.has_up);
	if (!has_up)
		startup_node(nid, &addr);

	init_node_memmap(nid, addr, size);

	page = virt_to_page(addr);
	zone = page_zone(page);

	if (!has_up) {
		/*初始化时，页不是一个完整的最高阶连续页，故单页释放*/
		for (unsigned long start = (unsigned long)addr;
			 	start < end; start+= VPAGE_SIZE) {
			__free_pages_ok(page++, 0);
		}
#ifdef ZONE_DEBUG
		/*验证有效性*/
		int i = 0;
		for (i = 0; i < RESERVED_ORDER_PER_NODE; i++) {
			BUG_ON(zone->free_area[i].nr_free != 0);
		}
		while (i < MAX_ORDER-1) {
			BUG_ON(zone->free_area[i++].nr_free != 1);
		}
		BUG_ON(zone->free_area[i].nr_free != 0);
#endif
	} else {
		__free_pages_ok(page, ilog2(size >> VPAGE_SHIFT));
	}

	zone->spanned_pages += size>>VPAGE_SHIFT;
	log_debug("supply memory to buddy system, nid : [%d]...", nid);
}

/*映射一段内存，然后启动该段内存对应的node区域的虚拟页管理器*/
static inline void startup_first_node(void)
{
	void *addr = alloc_memblock(BUDDY_BLKSIZE);
	BUG_ON(!addr);
	__node_supply_memory(addr, BUDDY_BLKSIZE);
}

static void init_zone(struct zone *zone, struct pglist_data *pgdata)
{
	zone->free_pages = 0;
	zone->spanned_pages = 0;
	zone->pgdata = pgdata;
	zone_lock_init(zone);
	for (int i = 0; i < MAX_ORDER; i++) {
		struct free_area *free_area = &zone->free_area[i];
		free_area->nr_free = 0;
		INIT_LIST_HEAD(&free_area->free_list);
	}
}

static void init_pg_data(struct node_config node_cfg[])
{
	uint32_t nid;

	bitmap_zero(node_map.has_free, MAX_NUMNODES);
	bitmap_zero(node_map.has_up, MAX_NUMNODES);

	for_each_node(nid) {
		pg_data_t *pgdata = NODE_DATA(nid);
		pgdata->node_id = nid;
		/*渐进式分配页描述符使用的内存*/
		pgdata->mem_map = NULL;
		pgdata->nr_zones = MAX_NR_ZONES; /*暂时只使用0号zone*/
		pgdata->start_pfn = node_cfg[nid].start_pfn;

		for (int i = 0; i < MAX_NR_ZONES; i++) {
			init_zone(&pgdata->node_zones[i], pgdata);
		}
	}
}

void __setup_memory(void)
{
	struct node_config node_cfg[MAX_NUMNODES];

	/*判断静态参数是否正确*/
	BUILD_BUG_ON_NOT_POWER_OF_2(VPAGE_SIZE);
	BUILD_BUG_ON(VPAGE_SIZE < PAGE_SIZE);
	BUILD_BUG_ON(NODES_SHIFT > MAX_NODES_SHIFT);
	/*每次补充的内存不能太多*/
	BUILD_BUG_ON((BUDDY_BLKPAGES<<1)>VPAGES_PER_NODE);
	/*node的标志和page标志位不能重合*/
	BUILD_BUG_ON(PG_MAX_FLAG+MAX_NODES_SHIFT > BITS_PER_LONG);
	/*页描述符占用的内存不能太多*/
	BUILD_BUG_ON((BUDDY_BLKSIZE>>1)<(sizeof(struct vpage)*VPAGES_PER_NODE));
	
	big_lock();

	if (READ_ONCE(node_up)) {
		big_unlock();
		return;
	}

	log_info("VIRTUAL PAGE SIZE : %ld(%d)", VPAGE_SIZE, VPAGE_SHIFT);
	log_info("MAX VIRTUAL ADDRESS : %p, MAX PFN : %ld", (void*)VADDR_END,
			 MAX_NR_VPAGES - 1);

	init_node_config(node_cfg);
	init_pg_data(node_cfg);
	startup_first_node();
	smp_mb();
	WRITE_ONCE(node_up, true);
	big_unlock();

	return;
}

int node_supply_memory(int order)
{
	void *addr;
	unsigned long nid;

	ZONE_BUG_ON(!READ_ONCE(node_up));

	big_lock();
	/*加锁再次检查*/
	for_each_free_node(nid) {
		if(node_has_freepg((int)nid, order))
			goto out;
	}

	addr = alloc_memblock(BUDDY_BLKSIZE);
	if (skp_unlikely(!addr)) {
		log_warn("TOO MUCH PAGES WAS IN BUDDY SYSTEM, OUT OF MEMORY");
		big_unlock();
		return -ENOMEM;
	}
	__node_supply_memory(addr, BUDDY_BLKSIZE);
out:
	big_unlock();
	return (int)nid;
}

void node_reclaim_memory(struct vpage *page, int order)
{
	log_debug("reclaim memory from buddy system, nid [%d] ...",
		page_to_nid(page));

	for (int i = 0; i < 1U << order; i++) {
		__ClearPageInited(page + i);
	}

	os_munmap(page_to_virt(page), VPAGE_SIZE << order);
}
