/*
  Red Black Trees
  (C) 1999  Andrea Arcangeli <andrea@suse.de>
  (C) 2002  David Woodhouse <dwmw2@infradead.org>
  (C) 2012  Michel Lespinasse <walken@google.com>

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

  linux/lib/rbtree.c
*/
#include <skp/adt/rbtree.h>

#define	RB_RED		0
#define	RB_BLACK	1

#define __rb_parent(pc)    ((struct rbt_node *)(pc & ~3))

#define __rb_color(pc)     ((pc) & 1)
#define __rb_is_black(pc)  __rb_color(pc)
#define __rb_is_red(pc)    (!__rb_color(pc))
#define rb_color(rb)       __rb_color((rb)->__rb_parent_color)
#define rb_is_red(rb)      __rb_is_red((rb)->__rb_parent_color)
#define rb_is_black(rb)    __rb_is_black((rb)->__rb_parent_color)

#define rb_set_child(rl, rb) WRITE_ONCE((rl), (rb))

static inline void rb_set_parent(struct rbt_node *rb, struct rbt_node *p)
{
	rb->__rb_parent_color = rb_color(rb) | (unsigned long)p;
}

static inline void rb_set_parent_color(struct rbt_node *rb,
				       struct rbt_node *p, int color)
{
	rb->__rb_parent_color = (unsigned long)p | color;
}

static inline void
rb_change_child(struct rbt_node *old, struct rbt_node *_new_,
		  struct rbt_node *parent, struct rbt_root *root)
{
	if (parent) {
		if (parent->rb_left == old)
			rb_set_child(parent->rb_left, _new_);
		else
			rb_set_child(parent->rb_right, _new_);
	} else
		rb_set_child(root->rb_node, _new_);
}

static struct rbt_node *
__rb_erase_node(struct rbt_node *node, struct rbt_root *root,
		struct rbt_node **leftmost)
{
	struct rbt_node *child = node->rb_right;
	struct rbt_node *tmp = node->rb_left;
	struct rbt_node *parent, *rebalance;
	unsigned long pc;

	if (leftmost && node == *leftmost)
		*leftmost = rbt_next(node);

	if (!tmp) {
		/*
		 * Case 1: node to erase has no more than 1 child (easy!)
		 *
		 * Note that if there is one child it must be red due to 5)
		 * and node must be black due to 4). We adjust colors locally
		 * so as to bypass __rbt_erase_color() later on.
		 */
		pc = node->__rb_parent_color;
		parent = __rb_parent(pc);
		rb_change_child(node, child, parent, root);
		if (child) {
			child->__rb_parent_color = pc;
			rebalance = NULL;
		} else
			rebalance = __rb_is_black(pc) ? parent : NULL;
		tmp = parent;
	} else if (!child) {
		/* Still case 1, but this time the child is node->rb_left */
		tmp->__rb_parent_color = pc = node->__rb_parent_color;
		parent = __rb_parent(pc);
		rb_change_child(node, tmp, parent, root);
		rebalance = NULL;
		tmp = parent;
	} else {
		struct rbt_node *successor = child, *child2;

		tmp = child->rb_left;
		if (!tmp) {
			/*
			 * Case 2: node's successor is its right child
			 *
			 *    (n)          (s)
			 *    / \          / \
			 *  (x) (s)  ->  (x) (c)
			 *        \
			 *        (c)
			 */
			parent = successor;
			child2 = successor->rb_right;
		} else {
			/*
			 * Case 3: node's successor is leftmost under
			 * node's right child subtree
			 *
			 *    (n)          (s)
			 *    / \          / \
			 *  (x) (y)  ->  (x) (y)
			 *      /            /
			 *    (p)          (p)
			 *    /            /
			 *  (s)          (c)
			 *    \
			 *    (c)
			 */
			do {
				parent = successor;
				successor = tmp;
				tmp = tmp->rb_left;
			} while (tmp);
			child2 = successor->rb_right;
			rb_set_child(parent->rb_left, child2);
			rb_set_child(successor->rb_right, child);
			rb_set_parent(child, successor);
		}

		tmp = node->rb_left;
		rb_set_child(successor->rb_left, tmp);
		rb_set_parent(tmp, successor);

		pc = node->__rb_parent_color;
		tmp = __rb_parent(pc);
		rb_change_child(node, successor, tmp, root);

		if (child2) {
			successor->__rb_parent_color = pc;
			rb_set_parent_color(child2, parent, RB_BLACK);
			rebalance = NULL;
		} else {
			unsigned long pc2 = successor->__rb_parent_color;
			successor->__rb_parent_color = pc;
			rebalance = __rb_is_black(pc2) ? parent : NULL;
		}
		tmp = successor;
	}

	return rebalance;
}

/*
 * red-black trees properties:  http://en.wikipedia.org/wiki/Rbtree
 *
 *  1) A node is either red or black
 *  2) The root is black
 *  3) All leaves (NULL) are black
 *  4) Both children of every red node are black
 *  5) Every simple path from root to leaves contains the same number
 *     of black nodes.
 *
 *  4 and 5 give the O(log n) guarantee, since 4 implies you cannot have two
 *  consecutive red nodes in a path and every red node is therefore followed by
 *  a black. So if B is the number of black nodes on every simple path (as per
 *  5), then the longest possible path due to 4 is 2B.
 *
 *  We shall indicate color with case, where black nodes are uppercase and red
 *  nodes will be lowercase. Unknown color nodes shall be drawn as red within
 *  parentheses and have some accompanying text comment.
 */

/*
 * Notes on lockless lookups:
 *
 * All stores to the tree structure (rb_left and rb_right) must be done using
 * rb_set_child(). And we must not inadvertently cause (temporary) loops in the
 * tree structure as seen in program order.
 *
 * These two requirements will allow lockless iteration of the tree -- not
 * correct iteration mind you, tree rotations are not atomic so a lookup might
 * miss entire subtrees.
 *
 * But they do guarantee that any such traversal will only see valid elements
 * and that it will indeed complete -- does not get stuck in a loop.
 *
 * It also guarantees that if the lookup returns an element it is the 'correct'
 * one. But not returning an element does _NOT_ mean it's not present.
 *
 * NOTE:
 *
 * Stores to __rb_parent_color are not important for simple lookups so those
 * are left undone as of now. Nor did I check for loops involving parent
 * pointers.
 */

static inline void rb_set_black(struct rbt_node *rb)
{
	rb->__rb_parent_color |= RB_BLACK;
}

static inline struct rbt_node *rbt_red_parent(struct rbt_node *red)
{
	return (struct rbt_node *)red->__rb_parent_color;
}

/*
 * Helper function for rotations:
 * - old's parent and color get assigned to _new_
 * - old gets assigned _new_ as a parent and 'color' as a color.
 */
static inline void
rb_rotate_set_parents(struct rbt_node *old, struct rbt_node *_new_,
			struct rbt_root *root, int color)
{
	struct rbt_node *parent = rbt_parent(old);
	_new_->__rb_parent_color = old->__rb_parent_color;
	rb_set_parent_color(old, _new_, color);
	rb_change_child(old, _new_, parent, root);
}

static void
__rb_insert(struct rbt_node *node, struct rbt_root *root,
	bool _new_left, struct rbt_node **leftmost)
{
	struct rbt_node *parent = rbt_red_parent(node), *gparent, *tmp;

	if (_new_left)
		*leftmost = node;

	while (true) {
		/*
		 * Loop invariant: node is red.
		 */
		if (skp_unlikely(!parent)) {
			/*
			 * The inserted node is root. Either this is the
			 * first node, or we recursed at Case 1 below and
			 * are no longer violating 4).
			 */
			rb_set_parent_color(node, NULL, RB_BLACK);
			break;
		}

		/*
		 * If there is a black parent, we are done.
		 * Otherwise, take some corrective action as,
		 * per 4), we don't want a red root or two
		 * consecutive red nodes.
		 */
		if(rb_is_black(parent))
			break;

		gparent = rbt_red_parent(parent);

		tmp = gparent->rb_right;
		if (parent != tmp) {	/* parent == gparent->rb_left */
			if (tmp && rb_is_red(tmp)) {
				/*
				 * Case 1 - node's uncle is red (color flips).
				 *
				 *       G            g
				 *      / \          / \
				 *     p   u  -->   P   U
				 *    /            /
				 *   n            n
				 *
				 * However, since g's parent might be red, and
				 * 4) does not allow this, we need to recurse
				 * at g.
				 */
				rb_set_parent_color(tmp, gparent, RB_BLACK);
				rb_set_parent_color(parent, gparent, RB_BLACK);
				node = gparent;
				parent = rbt_parent(node);
				rb_set_parent_color(node, parent, RB_RED);
				continue;
			}

			tmp = parent->rb_right;
			if (node == tmp) {
				/*
				 * Case 2 - node's uncle is black and node is
				 * the parent's right child (left rotate at parent).
				 *
				 *      G             G
				 *     / \           / \
				 *    p   U  -->    n   U
				 *     \           /
				 *      n         p
				 *
				 * This still leaves us in violation of 4), the
				 * continuation into Case 3 will fix that.
				 */
				tmp = node->rb_left;
				rb_set_child(parent->rb_right, tmp);
				rb_set_child(node->rb_left, parent);
				if (tmp)
					rb_set_parent_color(tmp, parent,
							    RB_BLACK);
				rb_set_parent_color(parent, node, RB_RED);
				parent = node;
				tmp = node->rb_right;
			}

			/*
			 * Case 3 - node's uncle is black and node is
			 * the parent's left child (right rotate at gparent).
			 *
			 *        G           P
			 *       / \         / \
			 *      p   U  -->  n   g
			 *     /                 \
			 *    n                   U
			 */
			rb_set_child(gparent->rb_left, tmp); /* == parent->rb_right */
			rb_set_child(parent->rb_right, gparent);
			if (tmp)
				rb_set_parent_color(tmp, gparent, RB_BLACK);
			rb_rotate_set_parents(gparent, parent, root, RB_RED);
			break;
		} else {
			tmp = gparent->rb_left;
			if (tmp && rb_is_red(tmp)) {
				/* Case 1 - color flips */
				rb_set_parent_color(tmp, gparent, RB_BLACK);
				rb_set_parent_color(parent, gparent, RB_BLACK);
				node = gparent;
				parent = rbt_parent(node);
				rb_set_parent_color(node, parent, RB_RED);
				continue;
			}

			tmp = parent->rb_left;
			if (node == tmp) {
				/* Case 2 - right rotate at parent */
				tmp = node->rb_right;
				rb_set_child(parent->rb_left, tmp);
				rb_set_child(node->rb_right, parent);
				if (tmp)
					rb_set_parent_color(tmp, parent, RB_BLACK);
				rb_set_parent_color(parent, node, RB_RED);
				parent = node;
				tmp = node->rb_left;
			}

			/* Case 3 - left rotate at gparent */
			rb_set_child(gparent->rb_right, tmp); /* == parent->rb_left */
			rb_set_child(parent->rb_left, gparent);
			if (tmp)
				rb_set_parent_color(tmp, gparent, RB_BLACK);
			rb_rotate_set_parents(gparent, parent, root, RB_RED);
			break;
		}
	}
}

/*
 * Inline version for rbt_erase() use - we want to be able to inline
 * and eliminate the dummy_rotate callback there
 */
static void
__rb_erase_color(struct rbt_node *parent, struct rbt_root *root)
{
	struct rbt_node *node = NULL, *sibling, *tmp1, *tmp2;

	while (true) {
		/*
		 * Loop invariants:
		 * - node is black (or NULL on first iteration)
		 * - node is not the root (parent is not NULL)
		 * - All leaf paths going through parent and node have a
		 *   black node count that is 1 lower than other leaf paths.
		 */
		sibling = parent->rb_right;
		if (node != sibling) {	/* node == parent->rb_left */
			if (rb_is_red(sibling)) {
				/*
				 * Case 1 - left rotate at parent
				 *
				 *     P               S
				 *    / \             / \
				 *   N   s    -->    p   Sr
				 *      / \         / \
				 *     Sl  Sr      N   Sl
				 */
				tmp1 = sibling->rb_left;
				rb_set_child(parent->rb_right, tmp1);
				rb_set_child(sibling->rb_left, parent);
				rb_set_parent_color(tmp1, parent, RB_BLACK);
				rb_rotate_set_parents(parent, sibling, root, RB_RED);
				sibling = tmp1;
			}
			tmp1 = sibling->rb_right;
			if (!tmp1 || rb_is_black(tmp1)) {
				tmp2 = sibling->rb_left;
				if (!tmp2 || rb_is_black(tmp2)) {
					/*
					 * Case 2 - sibling color flip
					 * (p could be either color here)
					 *
					 *    (p)           (p)
					 *    / \           / \
					 *   N   S    -->  N   s
					 *      / \           / \
					 *     Sl  Sr        Sl  Sr
					 *
					 * This leaves us violating 5) which
					 * can be fixed by flipping p to black
					 * if it was red, or by recursing at p.
					 * p is red when coming from Case 1.
					 */
					rb_set_parent_color(sibling, parent,
							    RB_RED);
					if (rb_is_red(parent))
						rb_set_black(parent);
					else {
						node = parent;
						parent = rbt_parent(node);
						if (parent)
							continue;
					}
					break;
				}
				/*
				 * Case 3 - right rotate at sibling
				 * (p could be either color here)
				 *
				 *   (p)           (p)
				 *   / \           / \
				 *  N   S    -->  N   sl
				 *     / \             \
				 *    sl  Sr            S
				 *                       \
				 *                        Sr
				 *
				 * Note: p might be red, and then both
				 * p and sl are red after rotation(which
				 * breaks property 4). This is fixed in
				 * Case 4 (in __rbt_rotate_set_parents()
				 *         which set sl the color of p
				 *         and set p RB_BLACK)
				 *
				 *   (p)            (sl)
				 *   / \            /  \
				 *  N   sl   -->   P    S
				 *       \        /      \
				 *        S      N        Sr
				 *         \
				 *          Sr
				 */
				tmp1 = tmp2->rb_right;
				rb_set_child(sibling->rb_left, tmp1);
				rb_set_child(tmp2->rb_right, sibling);
				rb_set_child(parent->rb_right, tmp2);
				if (tmp1)
					rb_set_parent_color(tmp1, sibling,
							    RB_BLACK);
				tmp1 = sibling;
				sibling = tmp2;
			}
			/*
			 * Case 4 - left rotate at parent + color flips
			 * (p and sl could be either color here.
			 *  After rotation, p becomes black, s acquires
			 *  p's color, and sl keeps its color)
			 *
			 *      (p)             (s)
			 *      / \             / \
			 *     N   S     -->   P   Sr
			 *        / \         / \
			 *      (sl) sr      N  (sl)
			 */
			tmp2 = sibling->rb_left;
			rb_set_child(parent->rb_right, tmp2);
			rb_set_child(sibling->rb_left, parent);
			rb_set_parent_color(tmp1, sibling, RB_BLACK);
			if (tmp2)
				rb_set_parent(tmp2, parent);
			rb_rotate_set_parents(parent, sibling, root, RB_BLACK);
			break;
		} else {
			sibling = parent->rb_left;
			if (rb_is_red(sibling)) {
				/* Case 1 - right rotate at parent */
				tmp1 = sibling->rb_right;
				rb_set_child(parent->rb_left, tmp1);
				rb_set_child(sibling->rb_right, parent);
				rb_set_parent_color(tmp1, parent, RB_BLACK);
				rb_rotate_set_parents(parent, sibling, root, RB_RED);
				sibling = tmp1;
			}
			tmp1 = sibling->rb_left;
			if (!tmp1 || rb_is_black(tmp1)) {
				tmp2 = sibling->rb_right;
				if (!tmp2 || rb_is_black(tmp2)) {
					/* Case 2 - sibling color flip */
					rb_set_parent_color(sibling, parent, RB_RED);
					if (rb_is_red(parent))
						rb_set_black(parent);
					else {
						node = parent;
						parent = rbt_parent(node);
						if (parent)
							continue;
					}
					break;
				}
				/* Case 3 - left rotate at sibling */
				tmp1 = tmp2->rb_left;
				rb_set_child(sibling->rb_right, tmp1);
				rb_set_child(tmp2->rb_left, sibling);
				rb_set_child(parent->rb_left, tmp2);
				if (tmp1)
					rb_set_parent_color(tmp1, sibling, RB_BLACK);
				tmp1 = sibling;
				sibling = tmp2;
			}
			/* Case 4 - right rotate at parent + color flips */
			tmp2 = sibling->rb_right;
			rb_set_child(parent->rb_left, tmp2);
			rb_set_child(sibling->rb_right, parent);
			rb_set_parent_color(tmp1, sibling, RB_BLACK);
			if (tmp2)
				rb_set_parent(tmp2, parent);
			rb_rotate_set_parents(parent, sibling, root, RB_BLACK);
			break;
		}
	}
}

void rbt_insert_color(struct rbt_node *node, struct rbt_root *root)
{
	__rb_insert(node, root, false, NULL);
}

void rbt_erase(struct rbt_node *node, struct rbt_root *root)
{
	struct rbt_node *rebalance;
	rebalance = __rb_erase_node(node, root, NULL);
	if (rebalance)
		__rb_erase_color(rebalance, root);
}

void rbt_insert_color_cached(struct rbt_node *node,
		struct rbt_root_cached *root, bool leftmost)
{
	__rb_insert(node, &root->rb_root, leftmost, &root->rb_leftmost);
}

void rbt_erase_cached(struct rbt_node *node, struct rbt_root_cached *root)
{
	struct rbt_node *rebalance;
	rebalance = __rb_erase_node(node, &root->rb_root, &root->rb_leftmost);
	if (rebalance)
		__rb_erase_color(rebalance, &root->rb_root);
}

/*
 * This function returns the first node (in sort order) of the tree.
 */
struct rbt_node *rbt_first(const struct rbt_root *root)
{
	struct rbt_node	*n;

	n = root->rb_node;
	if (!n)
		return NULL;
	while (n->rb_left)
		n = n->rb_left;
	return n;
}

struct rbt_node *rbt_last(const struct rbt_root *root)
{
	struct rbt_node	*n;

	n = root->rb_node;
	if (!n)
		return NULL;
	while (n->rb_right)
		n = n->rb_right;
	return n;
}

struct rbt_node *rbt_next(const struct rbt_node *node)
{
	struct rbt_node *parent;

	if (RBT_EMPTY_NODE(node))
		return NULL;

	/*
	 * If we have a right-hand child, go down and then left as far
	 * as we can.
	 */
	if (node->rb_right) {
		node = node->rb_right;
		while (node->rb_left)
			node=node->rb_left;
		return (struct rbt_node *)node;
	}

	/*
	 * No right-hand children. Everything down and left is smaller than us,
	 * so any 'next' node must be in the general direction of our parent.
	 * Go up the tree; any time the ancestor is a right-hand child of its
	 * parent, keep going up. First time it's a left-hand child of its
	 * parent, said parent is our 'next' node.
	 */
	while ((parent = rbt_parent(node)) && node == parent->rb_right)
		node = parent;

	return parent;
}

struct rbt_node *rbt_prev(const struct rbt_node *node)
{
	struct rbt_node *parent;

	if (RBT_EMPTY_NODE(node))
		return NULL;

	/*
	 * If we have a left-hand child, go down and then right as far
	 * as we can.
	 */
	if (node->rb_left) {
		node = node->rb_left;
		while (node->rb_right)
			node=node->rb_right;
		return (struct rbt_node *)node;
	}

	/*
	 * No left-hand children. Go up till we find an ancestor which
	 * is a right-hand child of its parent.
	 */
	while ((parent = rbt_parent(node)) && node == parent->rb_left)
		node = parent;

	return parent;
}

void rbt_replace_node(struct rbt_node *victim, struct rbt_node *_new_,
		     struct rbt_root *root)
{
	struct rbt_node *parent = rbt_parent(victim);

	/* Copy the pointers/colour from the victim to the replacement */
	*_new_ = *victim;

	/* Set the surrounding nodes to point to the replacement */
	if (victim->rb_left)
		rb_set_parent(victim->rb_left, _new_);
	if (victim->rb_right)
		rb_set_parent(victim->rb_right, _new_);
	rb_change_child(victim, _new_, parent, root);
}

void rbt_replace_node_cached(struct rbt_node *victim, struct rbt_node *_new_,
			    struct rbt_root_cached *root)
{
	rbt_replace_node(victim, _new_, &root->rb_root);

	if (root->rb_leftmost == victim)
		root->rb_leftmost = _new_;
}

static struct rbt_node *rb_left_deepest_node(const struct rbt_node *node)
{
	for (;;) {
		if (node->rb_left)
			node = node->rb_left;
		else if (node->rb_right)
			node = node->rb_right;
		else
			return (struct rbt_node *)node;
	}
}

struct rbt_node *rbt_next_postorder(const struct rbt_node *node)
{
	const struct rbt_node *parent;
	if (!node)
		return NULL;
	parent = rbt_parent(node);

	/* If we're sitting on node, we've already seen our children */
	if (parent && node == parent->rb_left && parent->rb_right) {
		/* If we are the parent's left node, go to the parent's right
		 * node then all the way down to the left */
		return rb_left_deepest_node(parent->rb_right);
	} else
		/* Otherwise we are the parent's right node, and the parent
		 * should be next */
		return (struct rbt_node *)parent;
}

struct rbt_node *rbt_first_postorder(const struct rbt_root *root)
{
	if (!root->rb_node)
		return NULL;

	return rb_left_deepest_node(root->rb_node);
}

/*key唯一插入*/
static struct rbt_node *
__rbtree_insert_node(struct rbtree *tree,
		struct rbt_node *node, bool multi, bool update)
{
	int rc;
	bool leftmost = true;
	struct rbt_node **rb_link, *parent = NULL;
	struct rbt_root_cached *root = &tree->rb_root;
	const struct rbtree_ops *ops = tree->rb_ops;

	BUG_ON(!ops->compare_node);
	rb_link = &root->rb_root.rb_node;
	/*一直遍历，知道遇到叶子节点才停止，新节点只能链接到叶子节点*/
	while (*rb_link) {
		parent = *rb_link;
		prefetch(parent);
		rc = ops->compare_node(parent, node);
		if (rc > 0) {
			/*左*/
			rb_link = &parent->rb_left;
		} else {
			if (rc == 0 && !multi) {
				return parent;
			}
			leftmost = false;
			/*右*/
			rb_link = &parent->rb_right;
			/*即使是允许相同Key存在，也插入到最右边的位置*/
		}
	}
	/*
	 * parent 确定父节点
	 * rb_index 确定链接到父节点的右还是左孩子节点
	 */
	rbt_link_node(node, parent, rb_link);
	/*平衡树*/
	__rb_insert(node, &root->rb_root,
		update ? leftmost : false, &root->rb_leftmost);

	return NULL;
}

/*key唯一插入*/
struct rbt_node *rbtree_insert_node(struct rbtree* tree, struct rbt_node *node)
{
	return __rbtree_insert_node(tree, node, false, false);
}
/*key重复插入*/
void rbtree_multi_insert_node(struct rbtree* tree, struct rbt_node *node)
{
	__rbtree_insert_node(tree, node, true, false);
}

/*key唯一插入*/
struct rbt_node *rbtree_insert_node_cached(struct rbtree* tree,
		struct rbt_node *node)
{
	return __rbtree_insert_node(tree, node, false, true);
}

/*key重复插入*/
void rbtree_multi_insert_node_cached(struct rbtree* tree, struct rbt_node *node)
{
	__rbtree_insert_node(tree, node, true, true);
}

/*如果是重复key的容器，可以对以找到的值使用 rb_next() 进行检索*/
struct rbt_node * rbtree_lookup(struct rbtree* tree, const void *key)
{
	int rc;
	struct rbt_node *next;
	struct rbt_root_cached *root = &tree->rb_root;
	const struct rbtree_ops *ops = tree->rb_ops;

	BUG_ON(!ops->compare_key);
	next = root->rb_root.rb_node;
	while (next) {
		rc = ops->compare_key(next, key);
		if (rc < 0) {
			next = next->rb_right;
		} else {
			if (!rc)
				break;
			next = next->rb_left;
		}
	}

	return next;
}

/*检索第一个，用于 multi-key 的检索*/
struct rbt_node * rbtree_lookup_first(struct rbtree* tree, const void *key)
{
	const struct rbtree_ops *ops = tree->rb_ops;
	struct rbt_node *node, *last = rbtree_lookup(tree, key);
	BUG_ON(!ops->compare_key);
	while (last && (node = rbt_prev(last))) {
		if (ops->compare_node(last, node))
			break;
		last = node;
	}
	return last;
}

/*检索 multi-key 的下一个*/
struct rbt_node * rbtree_lookup_next(struct rbtree* tree, struct rbt_node *prev)
{
	struct rbt_node *node;
	const struct rbtree_ops *ops = tree->rb_ops;

	BUG_ON(!ops->compare_node);
	if (WARN_ON(!prev))
		return NULL;
	node = rbt_next(prev);
	if (node && !ops->compare_node(node, prev))
		return node;
	return NULL;
}

struct rbt_node * rbtree_lookup_last(struct rbtree* tree, const void *key)
{
	const struct rbtree_ops *ops = tree->rb_ops;
	struct rbt_node *node, *last = rbtree_lookup(tree, key);
	BUG_ON(!ops->compare_node);
	while (last && (node = rbt_next(last))) {
		if (ops->compare_node(last, node))
			break;
		last = node;
	}
	return last;
}

struct rbt_node * rbtree_lookup_prev(struct rbtree* tree, struct rbt_node *prev)
{
	struct rbt_node *node;
	const struct rbtree_ops *ops = tree->rb_ops;

	BUG_ON(!ops->compare_node);
	if (WARN_ON(!prev))
		return NULL;
	node = rbt_prev(prev);
	if (node && !ops->compare_node(node, prev))
		return node;
	return NULL;
}

/*按Key删除*/
struct rbt_node *rbtree_remove(struct rbtree* tree, const void *key)
{
	struct rbt_node *rebalance,
	*node = rbtree_lookup_first(tree, key);
	if (!node)
		return NULL;
	rebalance = __rb_erase_node(node, &tree->rb_root.rb_root, NULL);
	if (rebalance)
		__rb_erase_color(rebalance, &tree->rb_root.rb_root);
	RBT_CLEAR_NODE(node);
	return node;
}

struct rbt_node * rbtree_remove_cached(struct rbtree* tree, const void *key)
{
	struct rbt_node *rebalance,
	*node = rbtree_lookup_first(tree, key);
	if (!node)
		return NULL;
	rebalance = __rb_erase_node(node, &tree->rb_root.rb_root,
					&tree->rb_root.rb_leftmost);
	if (rebalance)
		__rb_erase_color(rebalance, &tree->rb_root.rb_root);
	RBT_CLEAR_NODE(node);
	return node;
}

void rbtree_remove_node(struct rbtree *tree, struct rbt_node *node)
{
	struct rbt_node *rebalance =
		__rb_erase_node(node, &tree->rb_root.rb_root, NULL);
	if (rebalance)
		__rb_erase_color(rebalance, &tree->rb_root.rb_root);
	RBT_CLEAR_NODE(node);
}

void rbtree_remove_node_cached(struct rbtree *tree, struct rbt_node *node)
{
	struct rbt_node *rebalance =
		__rb_erase_node(node, &tree->rb_root.rb_root,
				&tree->rb_root.rb_leftmost);
	if (rebalance)
		__rb_erase_color(rebalance, &tree->rb_root.rb_root);
	RBT_CLEAR_NODE(node);
}
