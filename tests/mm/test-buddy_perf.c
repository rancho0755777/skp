//
//  pages_thread_test.c
//  test
//
//  Created by 周凯 on 2019/3/12.
//  Copyright © 2019 zhoukai. All rights reserved.
//

#include <skp/utils/utils.h>
#include <skp/mm/pgalloc.h>
#include <skp/process/thread.h>

static int order = 2;
static int count = 12;
static int isrand = 0;

#define TEST_FREE

#define PER_SIZE (1024)

static int thread_cb(void *arg)
{
	struct {
		void *ptr;
		int order;
	} page_info[PER_SIZE] = { };

	uint64_t start, end;

	start = similar_abstime(0, 0);

	for (int i = 0; i < count; i++) {
		int c;
		for (c = 0; c < ARRAY_SIZE(page_info); c++) {
			page_info[c].order = (!isrand) ? order : (int)prandom_int(0, order);
			page_info[c].ptr = __get_free_pages(0, page_info[c].order);
			if (WARN_ON(!page_info[c].ptr))
				break;
		}

#ifdef TEST_FREE
		for (int j = 0; j < c; j++) {
			free_pages(page_info[j].ptr, page_info[j].order);
		}
#endif
	}

	end = similar_abstime(0, 0);

	log_info("alloc/free %u times, spend %.2lf ns/per", count * ARRAY_SIZE(page_info),
			 ((double)(end - start)) / (count * ARRAY_SIZE(page_info)));

	return 0;
}

int main(int argc, char **argv)
{
	int cpu;
	DEFINE_PER_CPU(uthread_t, threads);

	if (argc > 1)
		order = atoi(argv[1]);
	if (argc > 2)
		count = atoi(argv[2]);
	if (argc > 3)
		isrand = atoi(argv[3]);

	order = clamp(order, 0, MAX_ORDER - 1);
	count = clamp(count, 0, 20);

	count = 1 << count;

	log_info("test param : block %ld MBytes, times %d, rand : %s",
			 (VPAGE_SIZE << order) >> 20, count, isrand ? "Yes" : "No");

	for_each_possible_cpu(cpu) {
		per_cpu(threads, cpu) = uthread_create(thread_cb, 0);
		BUG_ON(!per_cpu(threads, cpu));
	}

	for_each_possible_cpu(cpu) {
		uthread_wakeup(per_cpu(threads, cpu));
	}

	for_each_possible_cpu(cpu) {
		uthread_stop(per_cpu(threads, cpu), NULL);
	}

	return EXIT_SUCCESS;
}
