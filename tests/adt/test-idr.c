//
//  idr_test.c
//  test
//
//  Created by 周凯 on 2018/11/4.
//  Copyright © 2018 zhoukai. All rights reserved.
//

#include <stdio.h>
#include <skp/utils/rwlock.h>
#include <skp/adt/idr.h>
#include <skp/process/thread.h>

static DEFINE_RWLOCK(idr_lock);
static struct idr idr_set;

#define IDR_DATA ((void*)0xdead)

static void benchmark_test(void)
{
	void *ptr;
	int rc;
	struct idr idr;
	uint32_t id, start = 128, end = BITS_PER_PAGE + BITS_PER_PAGE / 2 + 128;

	rc = idr_init_base(&idr, start, end);
	BUG_ON(rc);

	for (int i = 0; i < idr.idt.nr_bit; i++) {
		id = idr_alloc(&idr, (void*)((uintptr_t)i + 1));
		BUG_ON(id < 0);
	}

	for (int i = 0; i < idr.idt.nr_bit; i++) {
		ptr = idr_find(&idr, i + idr.idt.offset);
		BUG_ON(ptr != (void*)((uintptr_t)i + 1));
	}

	for (int i = 0; i < BITS_PER_PAGE / 2; i++) {
		ptr = idr_remove(&idr, i + idr.idt.offset);
		BUG_ON(ptr != (void*)((uintptr_t)i + 1));
	}

	idr_destroy(&idr);
}

#define NR_TESTS (1U<<22)

static int insert_test(void *arg)
{
	int id;

	for (int i = 0; i < NR_TESTS; i++) {
		write_lock(&idr_lock);
		id = idr_alloc(&idr_set, IDR_DATA);
		write_unlock(&idr_lock);
		if (skp_unlikely(id < 0)) {
			sched_yield();
			continue;
		}
	}

	return 0;
}

static int find_test(void *arg)
{
	void *ptr;
	uint32_t id;

	for (int i = 0; i < NR_TESTS; i++) {
		id = prandom_int(0, U16_MAX);

		read_lock(&idr_lock);
		ptr = idr_find(&idr_set, id);
		read_unlock(&idr_lock);

		if (ptr != NULL) {
			//log_debug("find other thread insertd data : %lld", id);
			BUG_ON(ptr != IDR_DATA);
			write_lock(&idr_lock);
			ptr = idr_remove(&idr_set, id);
			write_unlock(&idr_lock);
			WARN_ON(ptr != IDR_DATA);
		}
	}

	return 0;
}

static int idt_test(void *arg)
{
	int32_t id;

	for (int i = 0; i < NR_TESTS; i++) {
		id = idt_alloc(&idr_set.idt);
		if (skp_likely(id > -1)) {
			usleep(1);
			WARN_ON(!idt_remove(&idr_set.idt, id));
		}
	}

	return 0;
}

int main(int argc, char **argv)
{
	uthread_t insert_work[4];
	uthread_t find_work[4];
	uthread_t idt_work[4];

	log_info("benchmark start");
	benchmark_test();

	log_info("test start");
	BUG_ON(idr16_init(&idr_set));

	for (int i = 0; i < ARRAY_SIZE(insert_work); i++) {
		insert_work[i] = uthread_create(insert_test, 0);
	}

	for (int i = 0; i < ARRAY_SIZE(find_work); i++) {
		find_work[i] = uthread_create(find_test, 0);
	}

	for (int i = 0; i < ARRAY_SIZE(find_work); i++) {
		idt_work[i] = uthread_create(idt_test, 0);
	}

	for (int i = 0; i < ARRAY_SIZE(insert_work); i++) {
		uthread_wakeup(insert_work[i]);
		usleep(10);
	}

	for (int i = 0; i < ARRAY_SIZE(find_work); i++) {
		uthread_wakeup(find_work[i]);
		usleep(10);
	}

	for (int i = 0; i < ARRAY_SIZE(find_work); i++) {
		uthread_wakeup(idt_work[i]);
		usleep(10);
	}

	for (int i = 0; i < ARRAY_SIZE(insert_work); i++) {
		uthread_stop(insert_work[i], NULL);
	}

	for (int i = 0; i < ARRAY_SIZE(find_work); i++) {
		uthread_stop(find_work[i], NULL);
	}

	for (int i = 0; i < ARRAY_SIZE(idt_work); i++) {
		uthread_stop(idt_work[i], NULL);
	}

	idr_destroy(&idr_set);
	radix_tree_reclaim();

	log_info("test finish");
	return EXIT_SUCCESS;
}
