#include <skp/process/workqueue.h>

#define MAGIC (0xdeaddeadU)
#define NR_WORK_TRIGGER 1280

static struct workqueue_struct *mywq = NULL;

struct my_work {
	struct work_struct work;
	uint32_t magic;
	uint32_t nr_trigger;
	completion_t done;
};

static struct my_work work_array[1024];
static struct work_stat stat_array[ARRAY_SIZE(work_array)];

#define IDX(work) ((work) - work_array)

static void work_func(struct work_struct *__work)
{
	struct my_work *mywork =
		container_of(__work, struct my_work, work);

	BUG_ON(mywork->magic != MAGIC);

	mywork->nr_trigger++;

	work_finish_process(__work);

	/*自动唤醒所在的线程池中的其他工作线程处理后续任务 */

	if (prandom_chance(1.0f/50))
		usleep_unintr(1000);
/*
	if (prandom_chance(1.0f/10000))
		log_info("execute work on worker pool %p", mywork);
 */

	work_acc_stat(&stat_array[IDX(mywork)], __work);

	if (mywork->nr_trigger < NR_WORK_TRIGGER) {
        WARN_ON(!queue_work(mywq, __work));
	} else {
		complete(&mywork->done);
	}
}

static inline void work_init(struct my_work *mywork)
{
	mywork->magic = MAGIC;
	mywork->nr_trigger = 0;
	init_completion(&mywork->done);
	INIT_WORK(&mywork->work, work_func);
	BUG_ON(!queue_work(mywq, &mywork->work));
}

static inline bool work_wait(struct my_work *mywork)
{
	return wait_for_completion_timeout(&mywork->done, 1000) > 0 ? true : false;
}

int main(int argc, char const *argv[])
{
	/* code */
	int nr = 2;

	mywq = alloc_workqueue("mywork", WQ_UNBOUND, 0);
	if (WARN_ON(!mywq))
		return EXIT_FAILURE;

	while (nr-- > 0) {
		for (int i = 0; i < ARRAY_SIZE(work_array); i++) {
			work_init(&work_array[i]);
		}

		for (int i = 0; i < ARRAY_SIZE(work_array); ) {
			if (work_wait(&work_array[i])) {
				i++;
			} else {
				flush_work(&work_array[i].work);
				flush_workqueue(mywq);
			}
		}
		for (int i = 0; i < ARRAY_SIZE(work_array); i++) {
			BUG_ON(work_array[i].nr_trigger != NR_WORK_TRIGGER);
#ifdef WQ_STAT
			printf("<%03d> dispatch cost : %lu, sched cost : %lu, process cost : %lu\n"
				   , i
				   , stat_array[i].dispatch_cost / NR_WORK_TRIGGER
				   , stat_array[i].sched_cost / NR_WORK_TRIGGER
				   , stat_array[i].process_cost / NR_WORK_TRIGGER
				   );
#endif
		}
		sleep(5);
	}

	//sleep_unintr(10);

	destroy_workqueue(mywq);

	return 0;
}
