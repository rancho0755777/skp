#include <skp/process/workqueue.h>

#define MAGIC (0xdeaddeadU)
#define NR_WORK_TRIGGER 1280000

struct my_work {
	struct work_struct work;
	uint32_t cpu;
	uint32_t magic;
	uint32_t nr_trigger;
	completion_t done;
};

static struct my_work work_array[4 * 32];
static struct work_stat stat_array[ARRAY_SIZE(work_array)];

#define IDX(work) ((work) - work_array)

static void work_func(struct work_struct *__work)
{
	struct my_work *mywork =
		container_of(__work, struct my_work, work);

	BUG_ON(mywork->magic != MAGIC);

	mywork->nr_trigger++;

/*
	if (prandom_chance(1.0f/50))
		usleep_unintr(1000);

	if (prandom_chance(1.0f/10000))
		log_info("execute work on worker pool %p [%lu]", mywork, (long)__work->private);
*/

	work_finish_process(__work);
	work_acc_stat(&stat_array[IDX(mywork)], __work);

	if (mywork->nr_trigger < NR_WORK_TRIGGER) {
		schedule_work_on(mywork->cpu, __work);
		//schedule_work_on(-1, __work);
	} else {
		complete(&mywork->done);
	}
}

static inline void work_init(struct my_work *mywork)
{
	static int idx = 0;
	mywork->magic = MAGIC;
	mywork->nr_trigger = 0;
	mywork->cpu = idx++ & (NR_CPUS - 1);
	init_completion(&mywork->done);
	INIT_WORK(&mywork->work, work_func);
	BUG_ON(!schedule_work_on(mywork->cpu, &mywork->work));
	//BUG_ON(!schedule_work(&mywork->work));
}

static inline bool work_wait(struct my_work *mywork)
{
	return wait_for_completion_timeout(&mywork->done, 1000) > 0 ? true : false;
}

int main(int argc, char const *argv[])
{
	/* code */

	log_info("start all of work");
	workqueue_init(true);

	for (int i = 0; i < ARRAY_SIZE(work_array); i++) {
		work_init(&work_array[i]);
	}

	for (int i = 0; i < ARRAY_SIZE(work_array); ) {
		if (work_wait(&work_array[i])) {
			i++;
		} else {
			flush_work(&work_array[i].work);
			flush_scheduled_work();
		}
	}

	log_info("finish all of work");

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

	//sleep_unintr(60);

	return 0;
}
