//
//  slab_thread_test.c
//
//  Created by 周凯 on 2019/2/28.
//  Copyright © 2019 zhoukai. All rights reserved.
//

#include <skp/utils/utils.h>
#include <skp/adt/list.h>
#include <skp/mm/pgalloc.h>
#include <skp/process/thread.h>

//#define JEMALLOC
//#define UMALLOC
//#define TCMALLOC

#ifdef JEMALLOC
# define JEMALLOC_MANGLE
# include <jemalloc/jemalloc.h>
# define malloc_implstr "jemalloc"
#elif defined TCMALLOC
# include <gperftools/tcmalloc.h>
# define malloc(s) tc_malloc((s))
# define free(p) tc_free((p))
# define malloc_implstr "tcmalloc"
#elif defined PTMALLOC
# define malloc_implstr "ptmalloc"
#else
# ifndef UMALLOC_MANGLE
#  define UMALLOC_MANGLE
# endif
# include <skp/mm/slab.h>
# define malloc_implstr "umalloc"
# ifdef UMEM_CACHE_TEST
static umem_cache_t *ALLOCATOR;
# define X_malloc(s) umem_cache_alloc(ALLOCATOR)
# endif
#endif

#define TEST_FREE 1

#ifndef X_malloc
# define X_malloc(x) malloc(x)
#endif

struct cache {
	int nr_tester;
	wait_queue_head_t wq;
	struct list_head queue;
};

static int order = 8;
static int count = 20;
static int isrand = 1;

#define MIN_OBJSIZE 16

static int free_cb(void *arg)
{
	int i = 0;
	int nr = 0;
	LIST__HEAD(queue);
	uint64_t start, end;
	struct list_head *c, *n;
	struct cache *cache = arg;
	DEFINE_AUTOREMOVE_WAITQUEUE(wait);

	start = similar_abstime(0, 0);

	do {
		prepare_to_wait(&cache->wq, &wait);

		if (list_empty_careful(&cache->queue)) {
			if (!READ_ONCE(cache->nr_tester))
				break;
			wait_on(&wait);
		}

		wait_queue_head_lock(&cache->wq);
		list_splice_tail_init(&cache->queue, &queue);
		wait_queue_head_unlock(&cache->wq);

		//i = 0;
		list_for_each_safe(c, n, &queue) {
			list_del(c);
			free(c);
			//i++;
			nr++;
		}
		//printf("free %d ...\n", i);

	} while (1);
	finish_wait(&cache->wq, &wait);

	end = similar_abstime(0, 0);

	if (nr)
		log_info("free %u times, spend %.2lf ns/per", nr,
			((double)(end - start)) / nr);

	return 0;
}

static int alloc_cb(void *arg)
{
	void *ptr;
	int nr = 0;
	LIST__HEAD(queue);
	uint64_t start, end;
	struct cache *cache = arg;
#ifndef UMEM_CACHE_TEST
	size_t l, size = 1U << order;
#endif

	start = similar_abstime(0, 0);

	for (int i = 0; i < count; i++) {
		for (int j = 0; j < 64; j++) {
#ifndef UMEM_CACHE_TEST
			l = (!isrand) ? size : prandom_int(MIN_OBJSIZE, size);
			l = roundup_pow_of_two(l);
			ptr = malloc(l);
			if (WARN_ON(!ptr)) {
				log_error("alloc failed : %zu", l);
				break;
			}
			/*挂上物理页*/
			if (l < PAGE_SIZE)
				memset(ptr, 1, l);
#else
			ptr = X_malloc(l);
#endif

#if 0
			if (prandom_chance(1.0f/4)) {
				free(ptr);
				nr++;
			} else
#endif
			{
				list_add_tail(ptr, &queue);
			}
			nr++;
		}

		wait_queue_head_lock(&cache->wq);
		list_splice_tail_init(&queue, &cache->queue);
		if (waitqueue_active(&cache->wq))
			wake_up_all_locked(&cache->wq);
		wait_queue_head_unlock(&cache->wq);
	}

	end = similar_abstime(0, 0);
	if (nr)
		log_info("alloc/free %u times, spend %.2lf ns/per", nr,
			 ((double)(end - start)) / nr);

	xadd(&cache->nr_tester, -1);
	wake_up_all(&cache->wq);
	return 0;
}

int main(int argc, char **argv)
{
	struct cache cache;
#ifdef UMEM_CACHE_TEST
	uthread_t free_thd[2];
	uthread_t alloc_thd[2];
#else
	uthread_t free_thd[NR_CPUS/2];
	uthread_t alloc_thd[TEST_FREE ? (NR_CPUS/2) : NR_CPUS];
#endif

	if (argc > 1)
		order = atoi(argv[1]);
	if (argc > 2)
		count = atoi(argv[2]);
	if (argc > 3)
		isrand = atoi(argv[3]);

	log_info("start initial ...");

	void *ptr = malloc(8);
	free(ptr);

	count = 1U << clamp(count, 0, 30);
#if defined(UMALLOC) && defined(UMEM_CACHE_TEST)
	log_info("test param : implement umem_cache, block %ld Bytes, times %d, rand : %s",
			 order, count, "No");
	ALLOCATOR = umem_cache_create("test", max(order, MIN_OBJSIZE), 0, 0);
	if (!ALLOCATOR) {
		log_error("can't create umem_cache, size : %u", 1U<<order);
		return EXIT_FAILURE;
	}
#else
	order = clamp(order, ilog2(MIN_OBJSIZE), VPAGE_SHIFT + MAX_ORDER - 1);
	log_info("test param : implement %s, block %ld Bytes, times %d, rand : %s",
			 malloc_implstr, 1UL << order, count, isrand ? "Yes" : "No");

#endif

	INIT_LIST_HEAD(&cache.queue);
	init_waitqueue_head(&cache.wq);
	cache.nr_tester = ARRAY_SIZE(alloc_thd);

////////////////////////////////////////////////////////////////////////////////
	for (int i = 0; TEST_FREE && i < ARRAY_SIZE(free_thd); i++) {
		free_thd[i] = uthread_create(free_cb, &cache);
		BUG_ON(!free_thd[i]);
	}

	for (int i = 0; i < ARRAY_SIZE(alloc_thd); i++) {
		alloc_thd[i] = uthread_create(alloc_cb, &cache);
		BUG_ON(!alloc_thd[i]);
	}
////////////////////////////////////////////////////////////////////////////////
	for (int i = 0; TEST_FREE && i < ARRAY_SIZE(free_thd); i++) {
		uthread_wakeup(free_thd[i]);
	}

	for (int i = 0; i < ARRAY_SIZE(alloc_thd); i++) {
		uthread_wakeup(alloc_thd[i]);
	}

////////////////////////////////////////////////////////////////////////////////
	for (int i = 0; i < ARRAY_SIZE(alloc_thd); i++) {
		uthread_stop(alloc_thd[i], NULL);
	}

	for (int i = 0; TEST_FREE && i < ARRAY_SIZE(free_thd); i++) {
		uthread_stop(free_thd[i], NULL);
	}

////////////////////////////////////////////////////////////////////////////////
	/*做一次清理*/
	free_cb(&cache);

#ifdef UMEM_CACHE_TEST
	umem_cache_destroy(ALLOCATOR);
#endif

#ifdef UMALLOC
	umem_cache_shrink_all();
#endif

	return EXIT_SUCCESS;
}
