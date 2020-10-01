//
//  mmcfg.h
//
//  Created by 周凯 on 2019/2/28.
//  Copyright © 2019 zhoukai. All rights reserved.
//

#ifndef __US_MMCFG_H__
#define __US_MMCFG_H__

/*node 最大数量*/
#define MAX_NODES_SHIFT 8

#ifndef CONFIG_VPAGE_SHIFT
# ifndef __x86_64__
#  define CONFIG_VPAGE_SHIFT 19
# else
#  define CONFIG_VPAGE_SHIFT 22
# endif
#endif

/*虚地址结束地址*/
#ifndef __x86_64__
# define VADDR_END (4UL << 30)
#else
# define VADDR_END (128UL << 40)
#endif

#ifndef CONFIG_NODES_SHIFT
# ifndef __x86_64__
#  define CONFIG_NODES_SHIFT 2 //ilog2(4)
# else
#  define CONFIG_NODES_SHIFT 8 //ilog2(256)
# endif
#endif

#define NODES_SHIFT CONFIG_NODES_SHIFT
#define MAX_NUMNODES (1U << NODES_SHIFT)
#define	NUMA_NO_NODE (-1)

/*定义虚拟页框大小*/
#define VPAGE_SHIFT CONFIG_VPAGE_SHIFT
#define VPAGE_SIZE (1UL << VPAGE_SHIFT)

#define MAX_NR_VPAGES (VADDR_END/VPAGE_SIZE)
#define VPAGES_PER_NODE (MAX_NR_VPAGES/MAX_NUMNODES)

/*最多分配 2^10 * VPAGE 的连续虚拟页框*/
#ifndef CONFIG_FORCE_MAX_ZONEORDER
# define MAX_ORDER 11
#else
# define MAX_ORDER CONFIG_FORCE_MAX_ZONEORDER
#endif

/*每次补充伙伴系统时分配的内存大小*/
#define BUDDY_BLKORDER (MAX_ORDER - 1)
#define BUDDY_BLKPAGES (1UL << BUDDY_BLKORDER)
#define BUDDY_BLKSIZE (BUDDY_BLKPAGES << VPAGE_SHIFT)

#define MAX_ELEMENTS (1U << MAX_NODES_SHIFT)
#define VPAGES_PER_ELEMENT (MAX_NR_VPAGES/MAX_ELEMENTS)
#define VPAGES_PER_ELEMENT_SHIFT ilog2(VPAGES_PER_ELEMENT)

//#define DEBUG_BUDDY
/*是否使用文件映射
#define CONFIG_BUDDY_FILEMMAP
#define CONFIG_BUDDY_SHAREDMMAP
 */

#endif /* __US_MMCFG_H__ */
