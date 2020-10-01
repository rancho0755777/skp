#ifndef __US_RBTHEAP_H__
#define __US_RBTHEAP_H__

/*
 * 使用红黑树实现堆
 */

#include "../utils/utils.h"
#include "rbtree.h"

__BEGIN_DECLS

struct heap_node;
/*返回 true, 则向红黑树的左节点检索*/
typedef bool (*heap_compare_fn)(struct heap_node *o, struct heap_node *n);

struct heap {
	struct rbt_root_cached root;
	heap_compare_fn compare_node;
};

struct heap_node {
	struct rbt_node rb;
};

#define __HEAP_NODE_INITIALIZER(n) {						\
	.rb = { .__rb_parent_color = (unsigned long)(&(n).rb) } \
}

#define DEFINE_HEAP_NODE(name)								\
	struct heap_node name =  __HEAP_NODE_INITIALIZER(name)

#define DEFINE_HEAP(name, compare)							\
	struct heap name = {									\
		.root = RBT_ROOT_CACHED, .compare_node = compare,	\
	}

static inline void heap_init(struct heap *heap, heap_compare_fn compare_node)
{
	heap->root.rb_leftmost = NULL;
	heap->root.rb_root.rb_node = NULL;
	heap->compare_node = compare_node;
}

#define heap_node_init(n) (RBT_CLEAR_NODE(&(n)->rb))
#define heap_node_empty(n) (!RBT_EMPTY_NODE(&(n)->rb))
#define heap_empty(heap) (RBT_EMPTY_ROOT(&(heap)->root.rb_root))

extern bool heap_insert(struct heap_node *node, struct heap *heap);
extern void heap_remove(struct heap_node* node, struct heap *heap);

#define heap_peek_entry(heap, type, member)							\
	({																\
		struct heap_node *__node = heap_peek((heap));				\
		__node ? container_of(__node, type, member) : NULL;			\
	})

/*获取第一个*/
static inline struct heap_node *heap_peek(struct heap *heap)
{
	return RBT_EMPTY_ROOT(&heap->root.rb_root) ? NULL :
		rbt_entry(rbt_first_cached(&heap->root), struct heap_node, rb);
}

#define heap_extract_entry(heap, type, member)						\
	({																\
		struct heap_node *__node = heap_extract((heap));			\
		skp_likely(__node) ? container_of(__node, type, member) : NULL; \
	})

/*移除第一个*/
static inline struct heap_node *heap_extract(struct heap *heap)
{
	struct heap_node *node = heap_peek(heap);
	if (skp_likely(node))
		heap_remove(node, heap);
	return node;
}

////////////////////////////////////////////////////////////////////////////////
// 整数大小堆
////////////////////////////////////////////////////////////////////////////////
struct heap_inode {
	struct heap_node node;
	int64_t value;
};

#define __HEAP_INODE_INITIALIZER(name, v) \
	{ .node = __HEAP_NODE_INITIALIZER(name.node), .value = v, }

#define DEFINE_HEAP_INODE(name, v) 	\
	struct heap_inode name =  __HEAP_INODE_INITIALIZER(name, v)

extern bool __iheap_less(struct heap_node *inheap, struct heap_node *_new_);
extern bool __iheap_greate(struct heap_node *inheap, struct heap_node *_new_);

/*整数大堆*/
#define DEFINE_MAXIHEAP(name) DEFINE_HEAP(name, __iheap_greate)
/*整数小堆*/
#define DEFINE_MINIHEAP(name) DEFINE_HEAP(name, __iheap_less)

static inline void heap_inode_init(struct heap_inode *node, int64_t value)
{
	heap_node_init(&node->node);
	node->value = value;
}

#define maxiheap_init(heap) heap_init(heap, __iheap_greate)
#define miniheap_init(heap) heap_init(heap, __iheap_less)

#define iheap_peek_entry(heap, type, member) 						\
({ 																	\
	struct heap_inode *__node = 									\
		heap_peek_entry((heap), struct heap_inode, node);			\
	__node ? container_of(__node, type, member) : NULL; 			\
})

#define iheap_extract_entry(heap, type, member)						\
	({																\
		struct heap_inode *__node =									\
			heap_extract_entry((heap), struct heap_inode, node);	\
		skp_likely(__node) ? container_of(__node, type, member) : NULL; \
	})

#define heap_inode_empty(n) heap_node_empty(&(n)->node)
#define iheap_insert(n, heap) heap_insert(&(n)->node, (heap))
#define iheap_remove(n, heap) heap_remove(&(n)->node, (heap))

static inline bool iheap_update(struct heap_inode *n, int64_t value,
		struct heap *heap)
{
	heap_remove(&n->node, heap);
	WRITE_ONCE(n->value, value);
	return heap_insert(&n->node, heap);
}


__END_DECLS


#endif
