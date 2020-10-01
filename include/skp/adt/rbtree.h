/*
  Red Black Trees
  (C) 1999  Andrea Arcangeli <andrea@suse.de>

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

  linux/include/linux/rbtree.h

  To use rbtrees you'll have to implement your own insert and search cores.
  This will avoid us to use callbacks and to drop drammatically performances.
  I know it's not the cleaner way,  but in C (not in C++) to get
  performances and genericity...

  Some example of insert and search follows here. The search is a plain
  normal search over an ordered tree. The insert instead must be implemented
  int two steps: as first thing the code must insert the element in
  order as a red leaf in the tree, then the support library function
  rbt_insert_color() must be called. Such function will do the
  not trivial work to rebalance the rbtree if necessary.

-----------------------------------------------------------------------
static inline struct page * rbt_search_page_cache(struct inode * inode,
						 unsigned long offset)
{
	struct rbt_node * n = inode->i_rbt_page_cache.rbt_node;
	struct page * page;

	while (n)
	{
		page = rbt_entry(n, struct page, rbt_page_cache);

		if (offset < page->offset)
			n = n->rb_left;
		else if (offset > page->offset)
			n = n->rb_right;
		else
			return page;
	}
	return NULL;
}

static inline struct page * __rbt_insert_page_cache(struct inode * inode,
						   unsigned long offset,
						   struct rbt_node * node)
{
	struct rbt_node ** p = &inode->i_rbt_page_cache.rbt_node;
	struct rbt_node * parent = NULL;
	struct page * page;

	while (*p)
	{
		parent = *p;
		page = rbt_entry(parent, struct page, rbt_page_cache);

		if (offset < page->offset)
			p = &(*p)->rb_left;
		else if (offset > page->offset)
			p = &(*p)->rb_right;
		else
			return page;
	}

	rbt_link_node(node, parent, p);

	return NULL;
}

static inline struct page * rbt_insert_page_cache(struct inode * inode,
						 unsigned long offset,
						 struct rbt_node * node)
{
	struct page * ret;
	if ((ret = __rbt_insert_page_cache(inode, offset, node)))
		goto out;
	rbt_insert_color(node, &inode->i_rbt_page_cache);
 out:
	return ret;
}
-----------------------------------------------------------------------
*/

#ifndef	__SU_RBTREE_H__
#define	__SU_RBTREE_H__

#include "../utils/utils.h"

__BEGIN_DECLS

struct rbt_node {
	unsigned long  __rb_parent_color;
	struct rbt_node *rb_right;
	struct rbt_node *rb_left;
} __aligned(sizeof(long));
    /* The alignment might seem pointless, but allegedly CRIS needs it */

struct rbt_root {
	struct rbt_node *rb_node;
};

/*
 * Leftmost-cached rbtrees.
 *
 * We do not cache the rightmost node based on footprint
 * size vs number of potential users that could benefit
 * from O(1) rbt_last(). Just not worth it, users that want
 * this feature can always implement the logic explicitly.
 * Furthermore, users that want to cache both pointers may
 * find it a bit asymmetric, but that's ok.
 */
struct rbt_root_cached {
	struct rbt_root rb_root;
	struct rbt_node *rb_leftmost;
};

#define rbt_parent(r)   ((struct rbt_node *)((r)->__rb_parent_color & ~3))

#define RBT_ROOT	(struct rbt_root) { NULL, }
#define RBT_ROOT_CACHED { {NULL, }, NULL }
#define	rbt_entry(ptr, type, member) container_of(ptr, type, member)

#define RBT_EMPTY_ROOT(root)  (READ_ONCE((root)->rb_node) == NULL)

/* 'empty' nodes are nodes that are known not to be inserted in an rbtree */
#define RBT_EMPTY_NODE(node)  \
	((node)->__rb_parent_color == (unsigned long)(node))
#define RBT_CLEAR_NODE(node)  \
	((node)->__rb_parent_color = (unsigned long)(node))


static inline void rbt_link_node(struct rbt_node *node,
		struct rbt_node *parent, struct rbt_node **rbt_link)
{
	node->__rb_parent_color = (unsigned long)parent;
	node->rb_left = node->rb_right = NULL;
	*rbt_link = node;
}

extern void rbt_insert_color(struct rbt_node *, struct rbt_root *);
extern void rbt_erase(struct rbt_node *, struct rbt_root *);


/* Find logical next and previous nodes in a tree */
extern struct rbt_node *rbt_next(const struct rbt_node *);
extern struct rbt_node *rbt_prev(const struct rbt_node *);
extern struct rbt_node *rbt_first(const struct rbt_root *);
extern struct rbt_node *rbt_last(const struct rbt_root *);

extern void rbt_insert_color_cached(struct rbt_node *,
				   struct rbt_root_cached *, bool);
extern void rbt_erase_cached(struct rbt_node *node, struct rbt_root_cached *);
/* Same as rbt_first(), but O(1) */
#define rbt_first_cached(root) ((root)->rb_leftmost)

/* Postorder iteration - always visit the parent after its children */
extern struct rbt_node *rbt_first_postorder(const struct rbt_root *);
extern struct rbt_node *rbt_next_postorder(const struct rbt_node *);

/* Fast replacement of a single node without remove/rebalance/add/rebalance */
extern void rbt_replace_node(struct rbt_node *victim, struct rbt_node *_new_,
			    struct rbt_root *root);
extern void rbt_replace_node_cached(struct rbt_node *victim, struct rbt_node *_new_,
				   struct rbt_root_cached *root);

#define rbt_entry_safe(ptr, type, member) \
	({ typeof(ptr) ____ptr = (ptr); \
	   ____ptr ? rbt_entry(____ptr, type, member) : NULL; \
	})

/**
 * rbtree_postorder_for_each_entry_safe - iterate in post-order over rbt_root of
 * given type allowing the backing memory of @pos to be invalidated
 *
 * @pos:	the 'type *' to use as a loop cursor.
 * @n:		another 'type *' to use as temporary storage
 * @root:	'rbt_root *' of the rbtree.
 * @field:	the name of the rbt_node field within 'type'.
 *
 * rbtree_postorder_for_each_entry_safe() provides a similar guarantee as
 * list_for_each_entry_safe() and allows the iteration to continue independent
 * of changes to @pos by the body of the loop.
 *
 * Note, however, that it cannot handle other modifications that re-order the
 * rbtree it is iterating over. This includes calling rbt_erase() on @pos, as
 * rbt_erase() may rebalance the tree, causing us to miss some nodes.
 */
#define rbtree_postorder_for_each_entry_safe(pos, n, root, field) \
	for (pos = rbt_entry_safe(rbt_first_postorder(root), typeof(*pos), field); \
	     pos && ({ n = rbt_entry_safe(rbt_next_postorder(&pos->field), \
			typeof(*pos), field); 1; }); \
	     pos = n)

#define rbtree_preorder_for_each_entry_safe(pos, n, root, field) \
	for (pos = rbt_entry_safe(rbt_first(root), typeof(*pos), field); \
	     pos && ({ n = rbt_entry_safe(rbt_next(&pos->field), \
			typeof(*pos), field); 1; }); \
	     pos = n)

/*
 * 通用的快速插入、查找函数
 * 使用 key 操作的必须实现 rb_ops->compare_key();
 * 使用 node 操作的必须实现 rb_ops->compare_node();
 */
/**
 * 共享内存红黑树操作函数集合定义
 */
struct rbtree_ops {
	/* compare 函数 小于 0 则向右子树检索，
	 * 等于 0 则相等，
	 * 大于 0 则向左子树检索
	 */
	int (*compare_key)(struct rbt_node* existed, const void *key);
	int (*compare_node)(struct rbt_node* existed, struct rbt_node*_new_);
};

struct rbtree {
	struct rbt_root_cached rb_root;
	const struct rbtree_ops *rb_ops;
};

#define DEFINE_RBTREE(name, ops) \
	struct rbtree name = { .rb_root = RBT_ROOT_CACHED, .rb_ops = ops }

#define RBTREE_ROOT(tree) (&(tree)->rb_root.rb_root)

static inline void rbtree_init(struct rbtree *tree,const struct rbtree_ops *ops)
{
	WARN_ON(!ops->compare_key);
	WARN_ON(!ops->compare_node);
	tree->rb_ops = ops;
	tree->rb_root.rb_leftmost = NULL;
	tree->rb_root.rb_root.rb_node = NULL;
}

/*key唯一插入*/
extern struct rbt_node *rbtree_insert_node(struct rbtree*, struct rbt_node *n);
/*key重复插入*/
extern void rbtree_multi_insert_node(struct rbtree*, struct rbt_node *n);

/*key唯一插入*/
extern struct rbt_node *rbtree_insert_node_cached(struct rbtree*,
		struct rbt_node *n);
/*key重复插入*/
extern void rbtree_multi_insert_node_cached(struct rbtree*, struct rbt_node *n);

/*如果是重复key的容器，可以对以找到的值使用 rb_next() 进行检索*/
extern struct rbt_node *rbtree_lookup(struct rbtree*, const void *k);

/*检索第一个，用于 multi-key 的检索*/
extern struct rbt_node *rbtree_lookup_first(struct rbtree*, const void *k);
/*检索 multi-key 的下一个*/
extern struct rbt_node *rbtree_lookup_next(struct rbtree*, struct rbt_node *p);

/*检索最后一个，用于 multi-key 的检索*/
extern struct rbt_node *rbtree_lookup_last(struct rbtree*, const void *k);
/*检索 multi-key 的上一个*/
extern struct rbt_node *rbtree_lookup_prev(struct rbtree*, struct rbt_node *p);

/*按Key删除*/
extern struct rbt_node *rbtree_remove(struct rbtree*, const void *k);
extern struct rbt_node *rbtree_remove_cached(struct rbtree*, const void *k);

extern void rbtree_remove_node(struct rbtree *, struct rbt_node *);
extern void rbtree_remove_node_cached(struct rbtree *, struct rbt_node *);

__END_DECLS

#endif
