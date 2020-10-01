//
//  slab_base_test.c
//
//  Created by 周凯 on 2019/3/4.
//  Copyright © 2019 zhoukai. All rights reserved.
//

#include <skp/utils/utils.h>
#include <skp/process/thread.h>
#include <skp/mm/mmcfg.h>
#include <skp/mm/slab.h>

struct cache {
	int nr_tester;
	wait_queue_head_t wq;
	struct list_head queue;
};

static int free_cb(void *arg)
{
	int nr = 0;
	LIST__HEAD(queue);
	struct list_head *c, *n;
	struct cache *cache = arg;
	DEFINE_AUTOREMOVE_WAITQUEUE(wait);

	do {
		prepare_to_wait_exclusive(&cache->wq, &wait);

		if (list_empty_careful(&cache->queue)) {
			if (!READ_ONCE(cache->nr_tester))
				break;
			wait_on(&wait);
		}
		wait_queue_head_lock(&cache->wq);
		list_splice_tail_init(&cache->queue, &queue);
		wait_queue_head_unlock(&cache->wq);

		list_for_each_safe(c, n, &queue) {
			list_del(c);
			free(c);
			nr++;
		}

	} while (1);
	finish_wait(&cache->wq, &wait);

	log_info("free %d times ...", nr);
	return 0;
}

static int alloc_cb(void *arg)
{
	void *ptr;
	LIST__HEAD(queue);
	struct cache *cache = arg;

	for (int i = 0; i < (1U<<16); i++) {
		for (int j = 0; j < 32; j++) {
			ptr = malloc(prandom_int(32, (32U<<10)));
			//ptr = malloc(32);
			if (WARN_ON(!ptr))
				break;
			list_add_tail(ptr, &queue);
		}
		wait_queue_head_lock(&cache->wq);
		list_splice_tail_init(&queue, &cache->queue);
		if (waitqueue_active(&cache->wq))
			wake_up_one_locked(&cache->wq);
		wait_queue_head_unlock(&cache->wq);
	}

	xadd(&cache->nr_tester, -1);
	wake_up_all(&cache->wq);

	return 0;
}

int main(int argc, char **argv)
{
	void *ptr;
	struct cache cache;
	uthread_t free_thd[2];
	uthread_t alloc_thd[2];
	umem_cache_t *allocater;

	//sleep(5);
	/*单线程测试 */
	for (int i = 0; i < 2; i++) {
		for (size_t s = 1; s < BUDDY_BLKSIZE; s <<= 1) {
			ptr = umalloc(s);
			BUG_ON(!ptr);
			ufree(ptr);
		}
		for (size_t s = 192; s < (2UL << 10); s += 100) {
			void *ptr_buff[129];
			allocater = umem_cache_create("test", s, 0, 0);
			for (int x = 0; x < 2; x++) {
				for (int y = 0; y < ARRAY_SIZE(ptr_buff); y++) {
					ptr = umem_cache_alloc(allocater);
					BUG_ON(!ptr);
					ptr_buff[y] = ptr;
				}

				for (int y = 0; y < ARRAY_SIZE(ptr_buff); y++) {
					ufree(ptr_buff[y]);
				}
			}

			ptr = umem_cache_alloc(allocater);
			BUG_ON(!ptr);

			ptr = urealloc(ptr, 192);
			BUG_ON(!ptr);

			ptr = urealloc(ptr, 96);
			BUG_ON(!ptr);

			ufree(ptr);
			umem_cache_destroy(allocater);
		}
	}

	/*多线程测试*/
	INIT_LIST_HEAD(&cache.queue);
	init_waitqueue_head(&cache.wq);
	cache.nr_tester = ARRAY_SIZE(alloc_thd);

	for (int i = 0; i < ARRAY_SIZE(free_thd); i++) {
		free_thd[i] = uthread_run(free_cb, &cache);
		BUG_ON(!free_thd[i]);
	}

	for (int i = 0; i < ARRAY_SIZE(alloc_thd); i++) {
		alloc_thd[i] = uthread_run(alloc_cb, &cache);
		BUG_ON(!alloc_thd[i]);
	}

	for (int i = 0; i < ARRAY_SIZE(alloc_thd); i++) {
		uthread_stop(alloc_thd[i], NULL);
	}

	for (int i = 0; i < ARRAY_SIZE(free_thd); i++) {
		uthread_stop(free_thd[i], NULL);
	}

#if 0
	/*泄漏测试*/
	for (int i = 0; i < 128; i++) {
		malloc(prandom_int(8, 1024));
	}
#endif

	umem_cache_shrink_all();

	return EXIT_SUCCESS;
}
