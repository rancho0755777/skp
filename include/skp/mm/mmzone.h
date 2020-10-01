//
//  mmzone.h
//
//  Created by 周凯 on 2019/2/27.
//  Copyright © 2019 zhoukai. All rights reserved.
//

#ifndef __US_MMZONE_H__
#define __US_MMZONE_H__

#include <pthread.h>

#include "../utils/utils.h"
#include "../utils/mutex.h"
#include "../utils/bitmap.h"
#include "../adt/list.h"

__BEGIN_DECLS

#include "mmcfg.h"

/*
 * 1. 每个zone都有一个free_area[]结构
 * 2. 每个node又有多种不同类型的zone
 * 3. 故伙伴系统是按不同节点、不同内存类型管理的连续的页框的
 */
struct free_area {
	struct list_head free_list;/**<
		* 每个链表元素指针都指向page.lru字段. */
	unsigned long nr_free;/**<链表中元素的数量 = 连续伙伴页的数量*/
};

struct vpage;
struct pglist_data;

enum {
	ZONE_NORMAL, /* 映射的内存会被交换*/
	//ZONE_MLOCK, /* 映射的内存不被交换*/
	MAX_NR_ZONES,
	ZONES_SHIFT = order_base_2_const(MAX_NR_ZONES),
};

#define zone_lock_init(x) mutex_init(&(x)->lock)
#define zone_lock(x) mutex_lock(&(x)->lock)
#define zone_unlock(x) mutex_unlock(&(x)->lock)

struct zone {
	mutex_t lock;/**<该描述本身的保护锁*/
	uint32_t free_pages;
	struct free_area free_area[MAX_ORDER];/**< 标记伙伴系统中的空闲页，这个数组的
		* 每一个元素中的链表由2^k个连续页的起始描述符
		* 串联组成，其中k对应着数组的下标
		*/
	struct pglist_data *pgdata; /**<所属内存节点*/
	unsigned long spanned_pages;	/**< 正被管理的虚拟页框总数 */
};

struct pglist_data {
	struct zone node_zones[MAX_NR_ZONES];/**<节点区域描述符数组*/
	uint32_t node_id;/**<节点标识符*/
	uint32_t nr_zones;/**<节点区域描述符的个数
				  * 表示 node_zones[] 数组前几个被初始化过
				  */
	struct vpage *mem_map;/**<属于该节点页框数组起始地址*/
	unsigned long start_pfn;/**<节点内在全局页框中第一个页框值*/
};

/*虚拟页管理结构的图信息*/
struct pglist_map {
	DECLARE_BITMAP(has_up, MAX_NUMNODES); /**< 是否已经启动*/
	DECLARE_BITMAP(has_free, MAX_NUMNODES); /**< 是否有空闲的页*/
};

/*虚拟页管理结构*/
typedef struct pglist_data pg_data_t;

extern bool node_up;
extern uint8_t physnode_map[];
extern struct pglist_map node_map;
extern struct pglist_data node_data[];
#define NODE_DATA(nid) (&node_data[nid])
#define NODE_ZONE(nid) (&NODE_DATA(nid)->node_zones[0])
#define ZONE_FREEAREA(nid, order) (&NODE_ZONE(nid)->free_area[order])

#define for_each_node(nid)			\
	for ((nid) = 0; (nid) < MAX_NUMNODES; (nid)++)

/*遍历有空闲内存的节点*/
#define for_each_free_node(nid)		\
	for_each_set_bit((nid), node_map.has_free, MAX_NUMNODES)

/*已启动node数量*/
#define nr_node_has_up()			\
	bitmap_weight(node_map.has_up, MAX_NUMNODES)

/**初始化，安装内存*/
extern void __setup_memory(void);
static inline void setup_memory(void)
{
	if (skp_likely(READ_ONCE(node_up)))
		return;
	__setup_memory();
}

/*查看是否有满足条件的空闲页*/
static __always_inline bool node_has_freepg(int nid, int order)
{
	for(int i = order; i < MAX_ORDER; i++) {
		if (ZONE_FREEAREA(nid, i)->nr_free)
			return true;
	}
	return false;
}

/*补充一些内存到伙伴系统中*/
extern int node_supply_memory(int order);
/*从伙伴系统中回收一些内存*/
extern void node_reclaim_memory(struct vpage *page, int order);

static inline uint32_t pfn_to_nid(unsigned long pfn)
{
	return physnode_map[pfn >> VPAGES_PER_ELEMENT_SHIFT];
}

static inline unsigned long virt_to_pfn(const void *ptr)
{
	return ((uintptr_t)ptr) >> VPAGE_SHIFT;
}

static inline uint32_t virt_to_nid(const void *ptr)
{
	uint32_t nid = pfn_to_nid(virt_to_pfn(ptr));
	BUG_ON(!test_bit(nid, node_map.has_up));
	return nid;
}

#define mem_map(nid)	(NODE_DATA(nid)->mem_map)
#define start_pfn(nid)	(NODE_DATA(nid)->start_pfn)
#define node_locnr(pfn, nid) ((pfn) - start_pfn(nid))

__END_DECLS

#endif /* __US_MMZONE_H__ */
