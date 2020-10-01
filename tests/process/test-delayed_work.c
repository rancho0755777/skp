#include <skp/process/workqueue.h>

struct my_work {
	char subclass[32];
	struct delayed_work work;
};

static void delayed_work_cb(struct work_struct *work)
{
	struct delayed_work *delayedwork = to_delayed_work(work);
	struct my_work *my_work = container_of(delayedwork, struct my_work, work);
	log_info("%s delayed work run ...", my_work->subclass);
	schedule_delayed_work(delayedwork, 1000);
}

int main(int argc, char **argv)
{
	struct my_work my_work = { .subclass = "oop work", };

	sysevent_init(true);
	
	INIT_DELAYED_WORK(&my_work.work, delayed_work_cb);

	schedule_delayed_work(&my_work.work, 1000);

	flush_delayed_work(&my_work.work);

	sleep(50);

	/*离开作用域时，一定要同步删除工作对象*/
	cancel_delayed_work_sync(&my_work.work);

	return 0;
}
