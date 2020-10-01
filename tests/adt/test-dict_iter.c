#include <skp/adt/dict.h>
#include <skp/utils/hash.h>
#include <skp/utils/bitmap.h>

struct data {
	struct dict_node dnode;
	long value;
};

static uint32_t data__calc_hash(const void *key)
{
	return __hash_long((long)key);
}

static int data__compare_key(struct dict_node *e, const void *key, uint32_t hv)
{
	struct data *data = dict_entry(e, struct data, dnode);
	return data->value == (long)key ? 0 : -1;
}
	/*node比较函数，用于rehash排序*/
static int data__compare_node(struct dict_node *e, struct dict_node *i)
{
	struct data *edata = dict_entry(e, struct data, dnode);
	struct data *idata = dict_entry(i, struct data, dnode);
	return edata->value == idata->value ? 0 : -1;
}

static const struct dict_ops dict_ops = {
	.calc_hash = data__calc_hash,
	.compare_key = data__compare_key,
	.compare_node = data__compare_node,
};

static void free_node(struct dict_node *dnode, void *user)
{
	(*(int*)user)--;
	free(dnode);
}

#define NR_TEST 129

int main(int argc, char const *argv[])
{
	int nr_nodes = 0;
	struct dict dict;
	struct data *data = NULL;
	struct dict_node *old;
	struct dict_attr attr = {
		.expand_ratio = 2,
		.reduce_ratio = 1,
		.init_size = 32,
		.ops = &dict_ops,
	};
	DECLARE_BITMAP(value_idx, NR_TEST);

	bitmap_zero(value_idx, NR_TEST);
	dict_init(&dict, &attr);

	for (int i = 0; i < NR_TEST; i++) {
		data = malloc(sizeof (*data));
		data->value = i;
		dict_node_init(&data->dnode);

		old = dict_insert(&dict, (void*)(uintptr_t)i, &data->dnode);
		BUG_ON(old);
		nr_nodes++;
		__set_bit(i, value_idx);
#if 1
		if (dict.rehashidx != -1) {
			for (int j = 0; j < 2 && i < NR_TEST - 1; j++) {
				data = malloc(sizeof (*data));
				data->value = ++i;
				dict_node_init(&data->dnode);

				old = dict_insert(&dict, (void*)(uintptr_t)i, &data->dnode);
				BUG_ON(old);
				nr_nodes++;
				__set_bit(i, value_idx);
			}
			break;
		}
#endif
	}

#if 1
	BUG_ON(bitmap_weight(value_idx, NR_TEST) > NR_TEST);
#else
	BUG_ON(bitmap_weight(value_idx, NR_TEST) != NR_TEST);
#endif
	dict_for_each_entry(data, &dict, struct data, dnode) {
		BUG_ON(!__test_and_clear_bit(data->value, value_idx));
		dict_direct_remove(&dict, &data->dnode);
		free_node(&data->dnode, &nr_nodes);
	}
	BUG_ON(bitmap_weight(value_idx, NR_TEST));

	BUG_ON(dict_nodes(&dict));
	dict_release(&dict, NULL, NULL);
	BUG_ON(nr_nodes);

	return 0;
}
