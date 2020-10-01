/*
 * Copyright (C) 2001 Momchil Velikov
 * Portions Copyright (C) 2001 Christoph Hellwig
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2, or (at
 * your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */
#ifndef __US_RADIXTREE_H__
#define __US_RADIXTREE_H__

#include "../utils/utils.h"

__BEGIN_DECLS

#define RADIX_TREE_TAGS 3

struct radix_tree_node;
struct radix_tree_root {
	/*todo:新增最大和最小，加速遍历？*/
	uint32_t height;/**< 树的高度*/
	uint32_t alloc_mm; /**< 内部使用的内存大小*/
	uint64_t nr_nodes; /**< 节点数量*/
	struct radix_tree_node	*rnode;/**第一层的子树节点，叶子节点的槽位存放外部对象的指针*/
};

#define RADIX_TREE_INIT	{			\
	.height = (0),					\
	.alloc_mm = (0),				\
	.nr_nodes = (0),				\
	.rnode = NULL,					\
}

#define RADIX_TREE(name) 			\
	struct radix_tree_root name = RADIX_TREE_INIT

#define INIT_RADIX_TREE(root)		\
	do {							\
		(root)->height = 0;			\
		(root)->nr_nodes = 0;		\
		(root)->alloc_mm = 0;		\
		(root)->rnode = NULL;		\
	} while (0)


typedef void (*radix_tree_fn)(void *ptr, uint64_t index, void *user);

static inline uint64_t radix_tree_elems(const struct radix_tree_root *root)
{
	return READ_ONCE(root->nr_nodes);
}

extern void radix_tree_release(struct radix_tree_root *, radix_tree_fn free, void*);

/**调用 radix_tree_insert() 加锁之前需要调用此函数来加速*/
extern void radix_tree_preload(void);
/**回收缓存*/
extern void radix_tree_reclaim(void);

extern int __radix_tree_insert(struct radix_tree_root *, uint64_t index,
		void *item, struct radix_tree_node ***pslot);
extern void *radix_tree_delete(struct radix_tree_root *, uint64_t index);

extern void *__radix_tree_lookup(struct radix_tree_root *, uint64_t index,
		struct radix_tree_node ***pslot);

static inline int radix_tree_insert(struct radix_tree_root *root,
		uint64_t index, void *item)
{
	return __radix_tree_insert(root, index, item, NULL);
}

static inline void *radix_tree_lookup(const struct radix_tree_root *root,
		uint64_t index)
{
	return __radix_tree_lookup((struct radix_tree_root*)root, index, NULL);
}

extern uint32_t __radix_tree_gang_lookup(const struct radix_tree_root *,
		uint64_t index, void **results, uint32_t max_items, uint64_t *next_idx);

static inline uint32_t radix_tree_gang_lookup(const struct radix_tree_root *root,
		uint64_t index, void **results, uint32_t max_items)
{
	return __radix_tree_gang_lookup(root, index, results, max_items, NULL);
}

/*所有 tag 的操作必须保证 index 下标有对应的值*/
extern void *radix_tree_tag_set(struct radix_tree_root *root, uint64_t index, int tag);
extern void *radix_tree_tag_clear(struct radix_tree_root *root, uint64_t index, int tag);
extern int radix_tree_tag_get(struct radix_tree_root *root, uint64_t index, int tag);
extern uint32_t __radix_tree_gang_lookup_tag(const struct radix_tree_root *root,
		uint64_t first_index, void **results, uint32_t max_items, int tag,
		uint64_t *pnext_index);

static inline uint32_t radix_tree_gang_lookup_tag(const struct radix_tree_root *root,
		uint64_t first_index, void **results, uint32_t max_items, int tag)
{
	return __radix_tree_gang_lookup_tag(root, first_index, results, max_items, tag, NULL);
}
extern bool radix_tree_tagged(struct radix_tree_root *root, int tag);

struct radix_tree_iter {
	void *value[64];
	uint64_t index;
	uint64_t next_index;
};

static inline void *
radix_tree_iter_init(struct radix_tree_iter *iter, uint64_t start)
{
	iter->index = 0;
	iter->next_index = start;
	return iter;
}

extern void *radix_tree_iter_next(const struct radix_tree_root *,
		struct radix_tree_iter *);
extern void *radix_tree_iter_next_tag(const struct radix_tree_root *,
		struct radix_tree_iter *, int tag);

/*获取 iter 当前的索引*/
#define radix_tree_iter_index() (__iter.index)

#define radix_tree_for_each(ptr, root, start, ...)			\
	for (struct radix_tree_iter __iter, *__piter =			\
			radix_tree_iter_init(&__iter, start);			\
	(ptr = radix_tree_iter_next((root), __piter)); __VA_ARGS__)

#define radix_tree_for_each_tagged(ptr, root, start, tag, ...)	\
	for (struct radix_tree_iter __iter, *__piter =			\
			radix_tree_iter_init(&__iter, (start));			\
	(ptr = radix_tree_iter_next_tag((root), __piter, (tag))); __VA_ARGS__)

__END_DECLS

#endif
