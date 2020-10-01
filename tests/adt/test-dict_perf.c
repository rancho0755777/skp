#include <skp/adt/dict.h>
#include <skp/adt/slist.h>
#include <skp/process/thread.h>
#include <skp/mm/slab.h>

static atomic_t nr_objs = ATOMIC_INIT(0);

static SLIST__HEAD(obj_cache);

struct object {
	union {
		struct dict_node dnode;
		struct slist_node cache;
	};
	unsigned long  value;
};

static void *obj_alloc(void *data)
{
	struct object *obj =
		slist_shift_entry(&obj_cache, struct object, cache);
	atomic_inc(&nr_objs);
	if (skp_unlikely(!obj)) {
		obj = malloc(sizeof(struct object));
		BUG_ON(!obj);
	}
	return obj;
}

static void obj_free(void *__obj)
{
	struct object *obj = __obj;
	atomic_dec(&nr_objs);
	INIT_SLIST_NODE(&obj->cache);
	slist_add_head(&obj_cache, &obj->cache);
}

#define node2obj(n) \
	container_of((n), struct object, dnode)

static uint32_t calc_hash(const void *key)
{
	return __hash_long((unsigned long)(uintptr_t)key);
	/*
	return jhash_2words((uint32_t)(long)key,
		(uint32_t)(((long)key) >> 32), (uint32_t)(long)__func__);
	*/
}

static int compare_key(struct dict_node *existed, const void *key, uint32_t hv)
{
	unsigned long value = (uintptr_t)key;
	struct object *obj = node2obj(existed);

	if (obj->value == value) {
#ifdef DEBUG
		BUG_ON(existed->hvalue != hv);
#endif
		return 0;
	}
	return -1;
}

static int compare_node(struct dict_node *existed, struct dict_node *insert)
{
	struct object *obj1 = node2obj(existed);
	struct object *obj2 = node2obj(insert);

	if (obj1->value == obj2->value) {
#ifdef DEBUG
		BUG_ON(existed->hvalue != insert->hvalue);
#endif
		return 0;
	}
	return -1;
}

enum test_type {
	INSERT = 0,
	LOOKUP = 1,
	REMOVE = 2,
	TYPE_MAX = 3,
};

struct test_param {
	uint32_t  depths;
	uint32_t init_size;
	uint32_t nr_tests;
	bool isrand[TYPE_MAX];
	unsigned long *data[TYPE_MAX]; /*2-dimensional*/

	float result[TYPE_MAX];
	uint32_t expands[TYPE_MAX];
	uint32_t reduces[TYPE_MAX];
	uint32_t cur_size[TYPE_MAX];
	uint32_t alloc_mm[TYPE_MAX];
	uint32_t max_nodes[TYPE_MAX];
	uint32_t avg_nodes[TYPE_MAX];

	struct dict dict;
};

struct test_group {
	bool isrand[TYPE_MAX];
};

#define MAX_NR_TESTS (1U << 30)


static void release_node_cb(struct dict_node *found, void *user_data)
{
	struct object *obj = node2obj(found);
	obj_free(obj);
}

#ifndef INIT_SIZE
# define INIT_SIZE 12
#endif

static const struct dict_ops dict_ops = {
	.compare_key = compare_key,
	.compare_node = compare_node,
	.calc_hash = calc_hash,
};

static void test_param_init(struct test_param *param,
	uint32_t depths, uint32_t nr_test, struct test_group *group)
{
	const struct dict_attr dattr = {
	    .expand_ratio = depths,
	    .reduce_ratio = 1, /*prevent to reduce rehash*/
	    .init_size = 1 << INIT_SIZE,
		.ops = &dict_ops,
	};

	prandom_seed(time(NULL));

	memset(param, 0, sizeof(*param));

	memcpy(param->isrand, group->isrand, sizeof(group->isrand));
	for (int i = 0; i < TYPE_MAX; i++) {
		param->data[i] = malloc(nr_test * sizeof(*param->data[0]));
		BUG_ON(!param->data[i]);
		if (group->isrand[i]) {
			for (uint32_t j = 0; j < nr_test; j++) {
#if 0
				param->data[i][j] = prandom_int(0, MAX_NR_TESTS);
#else
				param->data[i][j] = prandom_int(0, nr_test);
#endif
			}
		} else {
			for (uint32_t j = 0; j < nr_test; j++) {
				param->data[i][j] = j;
			}
		}
	}
	param->depths = depths;
	param->nr_tests = nr_test;
	param->init_size = dattr.init_size;
	BUG_ON(dict_init(&param->dict, &dattr));
}

#define LOG(fmt, ...) fprintf(stderr, fmt, ##__VA_ARGS__)

static void test_param_release(struct test_param *param)
{
	uint32_t objs = dict_nodes(&param->dict);
	log_debug("non-free object : %u/%u", objs, atomic_read(&nr_objs));
	BUG_ON(objs != atomic_read(&nr_objs));
#ifdef DEBUG
	dict_stats_print(&param->dict);
#endif
	dict_release(&param->dict, release_node_cb, param);
	BUG_ON(atomic_read(&nr_objs));

	for (int i = 0; i < TYPE_MAX; i++) {
		free(param->data[i]);
	}

	/*print result*/
	/*print result of test*/
LOG("\n\n"
	"+--------+-------+------+------------+-----------+-----------+---------+---------+-----------+-----------+--------------+-----------+\n");
LOG("|  type  | depth | rand | test times | buckets 1 | buckets 2 | expands | reduces | max depth | avg depth |    memory    |   spend   |\n");
LOG("+--------+-------+------+------------+-----------+-----------+---------+---------+-----------+-----------+--------------+-----------+\n");
	for (int i = 0; i < TYPE_MAX; i++) {
		LOG("| %6s | %5u | %4s | %10u | %9u | %9u | %7u | %7u | %9u | %9u | %12u | %9u |\n"
			, i == INSERT ? "insert" : (i == LOOKUP ? "lookup" : "remove")
			, param->depths
			, param->isrand[i] ? "yes" : "no", param->nr_tests
			, param->init_size, param->cur_size[i]
			, param->expands[i], param->reduces[i]
			, param->max_nodes[i], param->avg_nodes[i]
			, param->alloc_mm[i], (uint32_t)param->result[i]
		);
	}
LOG("+--------+-------+------+------------+-----------+-----------+---------+---------+-----------+-----------+--------------+-----------+\n");

}

static void test_account(struct test_param *param, int idx)
{
	uint32_t expands = 0, reduces = 0;
#ifdef DEBUG
	dict_stats_print(&param->dict);
#else
	dict_stats_colloect(&param->dict);
#endif

#ifdef DICT_DEBUG
	param->expands[idx] = param->dict.stats.nr_expands;
	param->reduces[idx] = param->dict.stats.nr_reduces;
#endif

	if (idx > INSERT) {
		expands = READ_ONCE(param->expands[idx - 1]);
		reduces = READ_ONCE(param->reduces[idx - 1]);
	}

	param->expands[idx] -= expands;
	param->reduces[idx] -= reduces;

	param->cur_size[idx] = dict_nodes(&param->dict);

#ifdef DICT_DEBUG
	param->max_nodes[idx] = param->dict.stats.max_nodes;
	param->avg_nodes[idx] = param->dict.stats.avg_nodes;
	param->alloc_mm[idx] = param->dict.stats.alloc_mm;
#endif
}

#define START_TEST(args, idx, value)                         \
	do {                                                     \
		uint64_t           __diffns;                         \
		double             __result;                         \
		struct timespec    __start, __end;                   \
		int                __idx = (idx);                    \
		struct test_param *__param = (args);                 \
		log_debug("start %s", __func__);                     \
		get_timestamp(&__start);                             \
		for (int __j = 0; __j < __param->nr_tests; __j++) {  \
			(value) = param->data[__idx][__j];

#define FINISH_TEST()                                        \
		}                                                    \
		log_debug("finish %s", __func__);                    \
		get_timestamp(&__end);                               \
		__diffns = timestamp_diff(&__end, &__start, NULL);   \
		__result = (double)__diffns / __param->nr_tests;     \
		log_debug("%s result : %.02lfns", __func__, __result); \
		__param->result[__idx] = (float)__result;            \
		test_account(__param, __idx);                        \
	}                                                        \
	while (0)

static void test_insert(struct test_param *param)
{
	struct object *obj;
	unsigned long value;
	struct dict_node *old;

	START_TEST(param, INSERT, value) {
		obj = obj_alloc(param);
		obj->value = value;
		dict_node_init(&obj->dnode);
		old = dict_insert(&param->dict,
				(void *)(uintptr_t)value, &obj->dnode);
		if (skp_unlikely(old)) {
#ifdef DEBUG
			if (!param->isrand[INSERT]) {
				BUG();
			}
#endif
			obj_free(obj);
		}
	} FINISH_TEST();

	return;
}
/*
static void lookup_success(struct dict_node *found, void *user_data)
{
	struct dict_node *user = user_data;
	user->value = found->value;
}
*/

static void test_lookup(struct test_param *param)
{
	unsigned long value;
	struct dict_node *old;

	START_TEST(param, LOOKUP, value) {
		old = dict_lookup(&param->dict, (void *)(uintptr_t)value);
#ifdef DEBUG
		if (skp_unlikely(!old && !param->isrand[LOOKUP] &&
				!param->isrand[INSERT])) {
			BUG();
		}
#endif
		dict_check_rehashing(&param->dict);
	} FINISH_TEST();

	return;
}

static void test_remove(struct test_param *param)
{
	unsigned long value;
	struct dict_node *node;

	START_TEST(param, REMOVE, value) {
		node = dict_lookup_remove(
				&param->dict, (void *)(uintptr_t)value);
#ifdef DEBUG
		if (skp_unlikely(!node && !param->isrand[REMOVE] &&
				!param->isrand[INSERT])) {
			BUG();
		} else
#endif
		if (node) {
			obj_free(node2obj(node));
		}
	} FINISH_TEST();

	return;
}

static void test_startup(struct test_param *param, int type)
{
	switch (type) {
		case INSERT:
			test_insert(param);
			break;
		case REMOVE:
			test_remove(param);
			break;
		default:
			test_lookup(param);
	}
}

int main(int argc, char const *argv[])
{
	uint32_t depths[] = { 2, 4, 8, /*16, 32,*/ };
	uint32_t nr_tests[] = { 1U << 16, 1U << 20, /*1U << 24, 1U << 27, 1U << 28,*/ };
	struct test_group group[] = {
		{ false, true, true, },
		{ true, true, true, },
		{ true, false, true, },
		{ false, false, true, },
		{ false, true, false, },
		{ true, true, false, },
		{ true, false, false, },
		{ false, false, false, },
	};
	struct test_param param;

	for (size_t i = 0; i < ARRAY_SIZE(depths); i++) {
		for (size_t j = 0; j < ARRAY_SIZE(nr_tests); j++) {
			for (size_t m = 0; m < ARRAY_SIZE(group); m++) {
				test_param_init(&param, depths[i], nr_tests[j], &group[m]);
				for (int k = 0; k < TYPE_MAX; k++)
					test_startup(&param, k);
				test_param_release(&param);
			}
		}
	}

	return 0;
}
