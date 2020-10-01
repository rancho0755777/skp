#include <skp/adt/radix_tree.h>
#include <skp/process/thread.h>
#include <skp/mm/slab.h>

#define MAX_NR_TESTS (99999999)

enum {
	INSERT = 0U,
	LOOKUP = 1U,
	REMOVE = 2U,
	TYPE_MAX = 3U,
};

struct test_param {
	bool isrand[TYPE_MAX];
	uint32_t nr_tests;
	unsigned long *data[TYPE_MAX]; /*2-dimensional*/
	float result[TYPE_MAX];
	uint32_t alloc_mm[TYPE_MAX];
	uint32_t heigth[TYPE_MAX];
	struct radix_tree_root rdtree;
};

struct test_group {
	bool isrand[TYPE_MAX];
};

static void test_param_init(struct test_param *param,
		uint32_t nr_test, struct test_group *group)
{
	prandom_seed(time(NULL));

	memset(param, 0, sizeof(*param));
	param->nr_tests = nr_test;
	INIT_RADIX_TREE(&param->rdtree);

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
}

#define LOG(fmt, ...) fprintf(stderr, fmt, ##__VA_ARGS__)

static void test_param_release(struct test_param *param)
{
	for (uint32_t i = 0; i < param->nr_tests; i++) {
		radix_tree_delete(&param->rdtree, param->data[INSERT][i]);
	}

	BUG_ON(param->rdtree.rnode);

	for (int i = 0; i < TYPE_MAX; i++) {
		free(param->data[i]);
	}
/*print result*/
	/*print result of test*/
	LOG("\n\n"
	    "+--------+------+------------+--------+--------------+-----------+\n");
	LOG("|  type  | rand | test times | height |    memory    |   spend   |\n");
	LOG("+--------+------+------------+--------+--------------+-----------+\n");

	

	for (int i = 0; i < TYPE_MAX; i++) {
	LOG("| %6s | %4s | %10u | %6u | %12u | %9u |\n"
		, i == INSERT ? "insert" : (i == LOOKUP ? "lookup" : "remove")
		, param->isrand[i] ? "yes" : "no", param->nr_tests, param->heigth[i]
		, param->alloc_mm[i], (uint32_t)param->result[i]
	);
	}
	LOG("+--------+------+------------+--------+--------------+-----------+\n");
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
			if (skp_unlikely(!(__j & 63)))                       \
			sched_yield();                                 	 \
		}                                                    \
		log_debug("finish %s", __func__);                    \
		get_timestamp(&__end);                               \
		__diffns = timestamp_diff(&__end, &__start, NULL);   \
		__result = (double)__diffns / __param->nr_tests;     \
		log_debug("%s result : %.02lfns", __func__, __result); \
		__param->result[__idx] = (float)__result;            \
		__param->heigth[__idx] = __param->rdtree.height;     \
		__param->alloc_mm[__idx] = __param->rdtree.alloc_mm;   \
	} while (0)

static void test_insert(struct test_param *param)
{
	int rc;
	unsigned long value;

	START_TEST(param, INSERT, value) {
		radix_tree_preload();
		rc = radix_tree_insert(&param->rdtree, value, (void*)(uintptr_t)(value + 1));
		if (skp_unlikely(rc)) {
			if (skp_unlikely(rc == -ENOMEM)) {
				log_warn("out of memory : %u", param->rdtree.alloc_mm);
			}
#ifdef DEBUG
			else if (!param->isrand[INSERT]) {
				BUG();
			}
#endif
		}
	} FINISH_TEST();

#ifdef DEBUG
	if (!param->isrand[INSERT])
		BUG_ON(param->rdtree.nr_nodes != param->nr_tests);
#endif

	return;
}

static void test_lookup(struct test_param *param)
{
	void *ptr;
	unsigned long value;

	START_TEST(param, LOOKUP, value) {
		ptr = radix_tree_lookup(&param->rdtree, value);
#ifdef DEBUG
		if (skp_unlikely(!ptr && !param->isrand[LOOKUP] &&
					 !param->isrand[INSERT])) {
			BUG();
		} else if (ptr) {
			BUG_ON(value != (uintptr_t)ptr - 1);
		}
#endif
	} FINISH_TEST();

	return;
}

static void test_remove(struct test_param *param)
{
	void *ptr;
	unsigned long value;

	START_TEST(param, REMOVE, value) {
		ptr = radix_tree_delete(&param->rdtree, value);
#ifdef DEBUG
		if (skp_unlikely(!ptr && !param->isrand[REMOVE] &&
					 !param->isrand[INSERT])) {
			BUG();
		} else if (ptr) {
			BUG_ON(value != (uintptr_t)ptr - 1);
		}
#endif
	} FINISH_TEST();

#ifdef DEBUG
	if (!param->isrand[INSERT] && !param->isrand[REMOVE])
		BUG_ON(param->rdtree.nr_nodes);
#endif
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

int main(int argc, char *argv[])
{
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

	for (int i = 0; i < ARRAY_SIZE(nr_tests); i++) {
		for (int j = 0; j < ARRAY_SIZE(group); j++) {
			test_param_init(&param, nr_tests[i], &group[j]);
			for (int k = 0; k < TYPE_MAX; k++)
				test_startup(&param, k);
			test_param_release(&param);
		}
	}

	radix_tree_reclaim();
	
	return EXIT_SUCCESS;
}
