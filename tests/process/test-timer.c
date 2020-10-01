#include <math.h>
#include <skp/adt/list.h>
#include <skp/utils/mutex.h>
#include <skp/process/event.h>
#include <skp/process/signal.h>
#include <skp/process/thread.h>

#include <skp/mm/slab.h>

struct my_timer {
	uint32_t id;
	bool restart;
	uint32_t triggers;
	struct list_head node;
	struct uev_timer timer;
};

static LIST__HEAD(timer_list);
static DEFINE_MUTEX(list_lock);
static uint32_t nr_timers = 0;
static atomic_t timer_id = ATOMIC_INIT(0);
static atomic_t nr_timedouts = ATOMIC_INIT(0);

static atomic_t deviation_table[] = {
	[ 0 ... 512 ] = ATOMIC_INIT(0),
};

#define MAX_TIMERS (1<<10)

#ifndef UNIT
# define UNIT (1)
#endif

static void timer_cb(struct uev_timer *__timer)
{
	int rc = 0;
	int32_t remain;
	struct my_timer *timer = container_of(__timer, struct my_timer, timer);

	timer->triggers++;
	remain = uev_timer_remain(&timer->timer);
	if (prandom_chance(1.0f/3000)) {
		log_info("timer trigger : %u(%s T %d, E %d R %d V %lld N %lld)", timer->id,
			timer->restart ? "true" : "false", timer->triggers,
			uev_timer_escapes(&timer->timer), remain,
			(long long)__timer->node.value/1000000,
			(long long)uev_timer_future(0)/1000000
		);
	}

/*calculate timedout deviation*/
	remain = clamp_t(uint32_t, abs(remain), 0, ARRAY_SIZE(deviation_table) - 1);
	atomic_inc(&deviation_table[remain]);
	atomic_inc(&nr_timedouts);

	if (timer->restart) {
/*restart*/
		rc = uev_timer_modify(__timer, timer->timer.expires);
		if (skp_unlikely(rc > 0))
			log_info("concurrent modify : %u/%u", timer->id, timer->triggers);
		BUG_ON(rc < 0);
		return;
	}

	rc = 0;
/*delete*/
	mutex_lock(&list_lock);
	if (!list_empty(&timer->node)) {
		rc = 1;
		nr_timers--;
		list_del_init(&timer->node);
	}
	mutex_unlock(&list_lock);

	if (rc) {
		rc = uev_timer_delete_async(__timer);
		if (skp_unlikely(rc > 0))
			log_info("concurrent delete : %u/%u", timer->id, timer->triggers);
		BUG_ON(rc < 0);
		free(timer);
	}
	return;
}

static void add_utimer(bool restart, uint32_t expires)
{
	struct my_timer * timer = malloc(sizeof(*timer));
	if (skp_unlikely(!timer))
		return;

	expires *= UNIT;

	timer->triggers = 0;
	timer->restart = restart;
	timer->id = atomic_add_return(1, &timer_id);
	INIT_LIST_HEAD(&timer->node);
	uev_timer_init(&timer->timer, timer_cb);

	log_debug("add timer : %p/%s", timer, restart ? "true" : "false");

	mutex_lock(&list_lock);
	nr_timers++;
	list_add(&timer->node, &timer_list);
	BUG_ON(uev_timer_add(&timer->timer, expires));
	mutex_unlock(&list_lock);
}

static void del_utimer(bool reverse)
{
	int rc;
	struct my_timer *timer;

	mutex_lock(&list_lock);
	if (list_empty(&timer_list)) {
		mutex_unlock(&list_lock);
		return;
	}

	nr_timers--;
	timer = reverse ? list_last_entry(&timer_list, struct my_timer, node) :
		list_first_entry(&timer_list, struct my_timer, node);
	list_del_init(&timer->node);
	mutex_unlock(&list_lock);

	rc = uev_timer_delete_sync(&timer->timer);
	if (!rc)
		log_info("concurrent delete : %u/%d", timer->id, timer->triggers);

	log_debug("delete timer : %d(%s, T %d, E %d, R %d)",
		timer->id, timer->restart ? "true" : "false", timer->triggers,
		uev_timer_escapes(&timer->timer), uev_timer_remain(&timer->timer),
		uev_ev_timer(&timer->timer));

	free(timer);
}

static int concurrent_modify_test(void *arg)
{
	int i, rc = 0;
	struct my_timer *timer;

	while (!uthread_should_stop()) {
		msleep_unintr(prandom_int(1, 5));
		i = prandom_int(10, 100);
		mutex_lock(&list_lock);
		list_for_each_entry_reverse(timer, &timer_list, node) {
			rc = __uev_timer_modify(&timer->timer, timer->timer.expires);
			if (!rc) {
				log_info("concurrent modify : %u/%d", timer->id,
					READ_ONCE(timer->triggers));
			}
			if (--i < 1)
				break;
		}
		mutex_unlock(&list_lock);
	}

	return 0;
}

static int concurrent_delete_test(void *arg)
{
	int i, count = 0;

	while (!uthread_should_stop()) {
		msleep_unintr(prandom_int(1, 5));

		i = prandom_int(10, 100);
		for (int j = 0; j < i; j++) {
			del_utimer(prandom_chance(1.0f/2));
		}

		msleep_unintr(prandom_int(1, 5));

		i = prandom_int(10, 100);
		for (int j = 0; j < i; j++) {
			add_utimer(prandom_chance(2.0f/5), prandom_int(10, 5000));
		}

		if (!(count++%200))
			log_info("current TIMERS %u", READ_ONCE(nr_timers));
	}

	return 0;
}

int main(int argc, char const *argv[])
{
	int timers = MAX_TIMERS;
	uthread_t delthd, addthd;

	signal_block_all(NULL);
	if (argc > 1)
		timers = atoi(argv[1]);

	timers = timers < 1 ? MAX_TIMERS : timers;

	for (int i = 0; i < timers; i++) {
		add_utimer(prandom_chance(1.0f/4), prandom_int(10, 5000));
	}

	log_info("insert finish");

	addthd = uthread_run(concurrent_modify_test, 0);
	delthd = uthread_run(concurrent_delete_test, 0);

	uev_sigwait(SIGINT);

	uthread_stop(addthd, 0);
	uthread_stop(delthd, 0);

	/*del all of timer*/
	while (!list_empty_careful(&timer_list)) {
		del_utimer(false);
	}

	if (WARN_ON(nr_timers))
		log_warn("it have %u timers", nr_timers);

{
	uint32_t nr;
	printf("deviation table : %u\n", atomic_read(&nr_timedouts));
	for (int i = 0; i < ARRAY_SIZE(deviation_table) - 1; i++) {
		nr = atomic_read(&deviation_table[i]);
		if (!nr)
			continue;
		printf(" = %-3d : %-9u, %-.4f%%\n", i, nr,
			(float)nr*100/atomic_read(&nr_timedouts));
	}
	nr = atomic_read(&deviation_table[ARRAY_SIZE(deviation_table) - 1]);
	printf(" > %-3d : %-9u, %-.4f%%\n", ARRAY_SIZE(deviation_table) - 1,
		nr, (float)nr*100/atomic_read(&nr_timedouts));
}

	signal_unblock_all(NULL);
	return 0;
}
