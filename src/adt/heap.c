#include <skp/adt/heap.h>

bool heap_insert(struct heap_node *node, struct heap *heap)
{
	bool leftmost = true;
	struct rbt_root_cached *root = &heap->root;
	struct rbt_node **pnode = &root->rb_root.rb_node, *parent = NULL;

	while (*pnode) {
		struct heap_node *existed = rbt_entry(*pnode, struct heap_node, rb);
		prefetch(existed);
		parent = *pnode;
		if (heap->compare_node(existed, node)) {
			pnode = &parent->rb_left;
		} else {
			pnode = &parent->rb_right;
			leftmost = false;
		}
	}

	rbt_link_node(&node->rb, parent, pnode);
	rbt_insert_color_cached(&node->rb, root, leftmost);
	return leftmost;
}

void heap_remove(struct heap_node* node, struct heap *heap)
{
#ifdef DEBUG
	BUG_ON(RBT_EMPTY_ROOT(&heap->root.rb_root));
	BUG_ON(RBT_EMPTY_NODE(&node->rb));
#endif

	rbt_erase_cached(&node->rb, &heap->root);
	static_mb();
	RBT_CLEAR_NODE(&node->rb);
}

#define to_inode(ptr) container_of((ptr), struct heap_inode, node)

bool __iheap_less(struct heap_node *old, struct heap_node *new)
{
	struct heap_inode *nnode = to_inode(new);
	struct heap_inode *onode = to_inode(old);
	return !!(nnode->value < onode->value);
}

bool __iheap_greate(struct heap_node *old, struct heap_node *new)
{
	struct heap_inode *nnode = to_inode(new);
	struct heap_inode *onode = to_inode(old);
	return !!(nnode->value > onode->value);
}
