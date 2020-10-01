#include <skp/adt/rbtree.h>
#include <skp/utils/utils.h>

struct int_node {
	struct rbt_node rbnode;
	int value;
};

static int nr_nodes = 0;

static void int_node_found(struct rbt_root *root, int value,
	struct rbt_node ***link, struct rbt_node **parent)
{
	struct rbt_node **__link, *__parent;
	struct int_node *node;

	__parent = NULL;
	__link = &root->rb_node;

	while(*__link) {
		__parent = *__link;
		node = rbt_entry(__parent, struct int_node, rbnode);
		if (node->value < value) {
			__link = &__parent->rb_right;
		} else {
			__link = &__parent->rb_left;
		}
	}

	*parent = __parent;
	*link = __link;
}

static void int_node_insert(struct rbt_root *root, struct int_node *node)
{
	struct rbt_node **link, *parent;

	int_node_found(root, node->value, &link, &parent);
	rbt_link_node(&node->rbnode, parent, link);
	rbt_insert_color(&node->rbnode, root);
}

static void int_node_remove(struct rbt_root *root)
{
	struct int_node *node;
	//struct int_node *node, *next;
	log_debug("tree node :");
	while ((node = rbt_entry_safe(root->rb_node, struct int_node, rbnode))) {
		rbt_erase(&node->rbnode, root);
		nr_nodes--;
		RBT_CLEAR_NODE(&node->rbnode);
	}
}

static void int_node_iter(struct rbt_root *root)
{
	struct int_node *node, *next;
	log_debug("tree node :");
	rbtree_preorder_for_each_entry_safe(node, next, root, rbnode) {
		log_debug("%d%c", node->value, next ? ',' : '\n');
		if (skp_likely(next))
			BUG_ON(next->value < node->value);
	}
}

#define SEQ (1<<20)

int main(int argc, char const *argv[])
{
	struct rbt_root rbroot = RBT_ROOT;
	int value;
	struct int_node *node = malloc(sizeof(*node) * SEQ);

	BUG_ON(!node);

	log_info("start rbtree test");
	for (int i = 0; i < SEQ; i++) {
		RBT_CLEAR_NODE(&node[i].rbnode);
		node[i].value = i;
		int_node_insert(&rbroot, &node[i]);
		nr_nodes++;
	}

	int_node_iter(&rbroot);

	for (int i = 0; i < min(100, SEQ); i++) {
		value = prandom_int(0, SEQ - 1);
		if (!RBT_EMPTY_NODE(&node[value].rbnode)) {
			log_debug("delete node : %d\n", value);
			rbt_erase(&node[value].rbnode, &rbroot);
			nr_nodes--;
			RBT_CLEAR_NODE(&node[value].rbnode);
		}
	}

	int_node_iter(&rbroot);

	int_node_remove(&rbroot);

	log_info("finish rbtree test");
	BUG_ON(nr_nodes);

	free(node);

	return 0;
}


