#include <skp/adt/rbtree.h>
#include <skp/mm/slab.h>

#define COMP >
#define FIRST_KEY U32_MAX

struct test_node {
	struct rbt_node rb;
	uint32_t key;
	uint32_t val;
};

static int perf_loops = 10;
static const int nodes = 1U << 20;
static const int per_nodes = 1U << 5;
static struct test_node *node_array;

static struct rbtree rbtree;
static const struct rbtree_ops rb_ops;

static int __compare_key(struct rbt_node *old, const void *pkey)
{
	uint32_t key = *(uint32_t*)pkey;
	struct test_node *node = rbt_entry(
		old, struct test_node, rb);
	return key == node->key ? 0 : 
		(node->key COMP key ? -1 : 1);
}

static int __compare_node(struct rbt_node *old, struct rbt_node *new)
{
	struct test_node *old_node = rbt_entry(
		old, struct test_node, rb);
	struct test_node *new_node = rbt_entry(
		new, struct test_node, rb);
	return old_node->key == new_node->key ? 0 :
		(old_node->key COMP new_node->key ? -1 : 1);
}

static const struct rbtree_ops rb_ops = {
	.compare_key = __compare_key,
	.compare_node = __compare_node,
};

#define for_each_test_node(n) \
	for (int __i = 0; __i < nodes && ({ n = &node_array[__i]; true;}); __i++)

static void init_data(void)
{
	struct test_node *node;
	if (!node_array) {
		node_array = calloc(nodes, sizeof(*node_array));
		BUG_ON(!node_array);
	}

	for_each_test_node(node) {
		node->key = prandom_int(0, U32_MAX);	
		node->val = prandom_int(0, U32_MAX);
		RBT_CLEAR_NODE(&node->rb);
	}
}

static void init_seq_data(int nr)
{
	int i = 0, key = 1;
	struct test_node *node;
	for_each_test_node(node) {
		node->key = key;
		if (++i == nr) {
			key++;
			i = 0;
		}
	}
}

static void modify_perf_test(void)
{
	struct test_node *node;
	struct rbt_node *rb;
	uint64_t start, end;

	start = abstime(0, 0);
	for (int i = 0; i < perf_loops; i++) {
		for_each_test_node(node) {
			rb = rbtree_insert_node(&rbtree, &node->rb);
			BUG_ON(rb && rb == &node->rb);
		}
		for_each_test_node(node) {
			rb = rbtree_remove(&rbtree, &node->key);
			BUG_ON(rb && rb != &node->rb);
		}
		BUG_ON(rbtree.rb_root.rb_root.rb_node);
	}
	end = abstime(0, 0);
	printf("--> test insert / delete %d times (%d) : %lu ns\n", perf_loops, nodes,
		   (unsigned long)(end - start) / (perf_loops * nodes * 2));
}

static void lookup_perf_test(void)
{
	struct test_node *node;
	uint64_t start, end;
	uint32_t *belookup;
	int nr = nodes / 4;
	nr = nr?nr:(nodes - 4);

	BUG_ON(nodes < 4);
	belookup = malloc(nr * sizeof(*belookup));
	BUG_ON(!belookup);

	for (int j = 0; j < nr; j++) {
		int index = prandom_int(2, nodes - 2);
		belookup[j] = node_array[index].key;
	}

	for_each_test_node(node) {
		rbtree_insert_node(&rbtree, &node->rb);
	}

	start = abstime(0, 0);
	for (int i = 0; i < perf_loops; i++) {
		for (int j = 0; j < nr; j++) {
			uint32_t key = belookup[j];
			if (j & 1) {
				BUG_ON(!rbtree_lookup(&rbtree, &key));
			} else {
				key += 1;
				rbtree_lookup(&rbtree, &key);
			}
		}
	}
	end = abstime(0, 0);
	printf("--> test lookup %d times (%d) : %lu ns\n", perf_loops, nr,
		   (unsigned long)(end - start) / (perf_loops * nr));

	for_each_test_node(node) {
		rbtree_remove(&rbtree, &node->key);
	}
}

static void traversal_inorder_test(void)
{
	struct rbt_node *rb;
	struct test_node *node, *next, *last;
	uint64_t start, end;

	for_each_test_node(node) {
		rb = rbtree_insert_node(&rbtree, &node->rb);
		BUG_ON(rb && rb == &node->rb);
	}
	rb = rbtree_insert_node(&rbtree, &node->rb);
	BUG_ON(!rb);

	start = abstime(0, 0);

	for (int i = 0; i < perf_loops; i++) {
		last = NULL;
		rbtree_preorder_for_each_entry_safe(node, next,
				&rbtree.rb_root.rb_root, rb) {
			if (skp_likely(last))
				BUG_ON(node->key COMP last->key);
			last = node;
		}
	}

	end = abstime(0, 0);
	printf("--> test inorder traversal %d times (%d) : %lu ns\n", perf_loops, nodes,
		   (unsigned long)(end - start) / (perf_loops * nodes));

	for_each_test_node(node) {
		rb = rbtree_remove(&rbtree, &node->key);
		BUG_ON(rb && rb != &node->rb);
	}
	BUG_ON(rbtree.rb_root.rb_root.rb_node);
}

static inline bool is_red(struct rbt_node *rb)
{
	return !(rb->__rb_parent_color & 1);
}

static int black_path_count(struct rbt_node *rb)
{
	int count;
	for (count = 0; rb; rb = rbt_parent(rb))
		count += !is_red(rb);
	return count;
}

static void check_postorder(int nr_nodes)
{
	struct test_node *cur, *n;
	int count = 0;
	rbtree_postorder_for_each_entry_safe(cur, n,
			&rbtree.rb_root.rb_root, rb)
		count++;

	WARN_ON(count != nr_nodes);
}

static void check(void)
{
	struct rbt_node *rb;
	int count = 0, blacks = 0;
	uint32_t prev_key = FIRST_KEY;
	struct rbt_root *root = &rbtree.rb_root.rb_root;

	for (rb = rbt_first(root); rb; rb = rbt_next(rb)) {
		struct test_node *node = container_of(rb, struct test_node, rb);
		WARN_ON(node->key COMP prev_key);
		WARN_ON(is_red(rb) && (!rbt_parent(rb) ||
			is_red(rbt_parent(rb))));
		if (!count)
			blacks = black_path_count(rb);
		else
			WARN_ON((!rb->rb_left || !rb->rb_right) &&
				blacks != black_path_count(rb));
		prev_key = node->key;
		count++;
	}

	check_postorder(count);
	WARN_ON(count < (1 << black_path_count(
		rbt_last(root))) - 1);
}

static void benchmark_test(void)
{
	int i, nr = 1000;
	struct test_node *node;

	i = 0;
	for_each_test_node(node) {
		check();
		rbtree_insert_node(&rbtree, &node->rb);
		if (++i > nr)
			break;
	}

	i = 0;
	for_each_test_node(node) {
		check();
		rbtree_remove(&rbtree, &node->key);
		if (++i > nr)
			break;
	}

	BUG_ON(rbtree.rb_root.rb_root.rb_node);
}

static void multikey_test(void)
{
	struct rbt_node *rb;
	uint32_t i, last_key = 0;
	struct test_node *node, *next;

	init_seq_data(per_nodes);

	for_each_test_node(node) {
		rbtree_multi_insert_node(&rbtree, &node->rb);
	}

	i = 0;
	next = NULL;
#if 0
	rbtree_preorder_for_each_entry_safe(node, next,
			&rbtree.rb_root.rb_root, rb) {
		if (i++ % per_nodes) {
			printf("%u, ", node->key);
		} else {
			printf("\nkey : ");
		}
	}
	printf("\n");
#endif

	last_key = node_array[0].key;
	do {
		rb = rbtree_lookup_first(&rbtree, &last_key);
		if (skp_unlikely(!rb)) {
			break;
		} else {
			uint32_t i = 1;
			while (1) {
				rb = rbtree_lookup_next(&rbtree, rb);
				if (skp_unlikely(!rb))
					break;
				node = container_of(rb, struct test_node, rb);
				BUG_ON(node->key != last_key);
				i++;
			}
			BUG_ON(i != per_nodes);
		}
		last_key++;
	} while (1);

	for_each_test_node(node) {
		rbtree_remove(&rbtree, &node->key);
	}

	BUG_ON(rbtree.rb_root.rb_root.rb_node);
}

int main(void)
{
	rbtree_init(&rbtree, &rb_ops);

	init_data();
	benchmark_test();
	traversal_inorder_test();
	lookup_perf_test();
	modify_perf_test();
	multikey_test();
	return EXIT_SUCCESS;
}
