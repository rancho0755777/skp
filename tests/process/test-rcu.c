#include <skp/process/signal.h>
#include <skp/process/event.h>
#include <skp/process/thread.h>
#include <skp/mm/slab.h>

static atomic_t nr_objs = ATOMIC_INIT(0);
static atomic_t deviation_table[] = {
	[ 0 ... 1024 ] = ATOMIC_INIT(0),
};

#define RCU_MAGIC 0xdeaddeadU

struct object {
	struct rcu_head rcu_head;
	uint32_t magic;
	uint64_t start;
};

#define NR_TESTS (1U << 20)

static bool has_stopped = false;

static void hdl_sig(int signo)
{
	log_info("stop ...");
	WRITE_ONCE(has_stopped, true);
}

static void rcu_cb(struct rcu_head *head)
{
	uint64_t diff = uev_timer_future(0);
	struct object *obj = container_of(head, struct object, rcu_head);

	BUG_ON(obj->magic != RCU_MAGIC);

	diff -= obj->start;
	if (diff < RCU_NS) {
		log_warn("RCU's deviation is too much : %lu / %u", diff, RCU_NS);
	} else {
		diff -= RCU_NS;
		diff /= 1000000;
		diff = clamp_t(uint64_t, diff, 0, ARRAY_SIZE(deviation_table) - 1);
		atomic_inc(&deviation_table[diff]);
	}

	if (prandom_chance(1.0f / 5000))
		log_info("free obj [%p], diff time : %lu", obj, diff);

	//log_debug("free object : %p", obj);
	free(obj);
	if (!atomic_dec_return(&nr_objs) && prandom_chance(1.0f / 5000)) {
		log_info("no cache any object");
	}
}

static int rcu_thread(void *arg)
{
	struct object *obj;
	for (int i = 0; i < NR_TESTS && !has_stopped; i++) {
		usleep(5);
		obj = malloc(sizeof(*obj));
		//log_debug("alloc object : %p", obj);
		obj->magic = RCU_MAGIC;
		obj->start = uev_timer_future(0);
		atomic_inc(&nr_objs);
		call_rcu_sched(&obj->rcu_head, rcu_cb);
	}

	return 0;
}

int main(int argc, char const *argv[])
{
	uthread_t thread[NR_CPUS];

	sysevent_init(false);
	
	signal_setup(SIGINT, hdl_sig);

	for (int i = 0; i < NR_CPUS; i++) {
		thread[i] = uthread_run(rcu_thread, 0);
		BUG_ON(!thread[i]);
	}

	for (int i = 0; i < NR_CPUS; i++) {
		uthread_stop(thread[i], 0);
	}

	log_info("remain [%u] object has not been free", atomic_read(&nr_objs));

	//__cond_load_acquire(&nr_objs.counter, VAL == 0);

	while (nr_objs.counter && !has_stopped) {
		usleep(5000);
	}

{
	uint32_t nr;
	printf("deviation table : \n");
	for (int i = 0; i < ARRAY_SIZE(deviation_table) - 1; i++) {
		nr = atomic_read(&deviation_table[i]);
		if (!nr)
			continue;
		printf(" = %-3d : %-5u, %-.2f\n", i, nr, (float)nr/NR_TESTS);
	}
	nr = atomic_read(&deviation_table[ARRAY_SIZE(deviation_table) - 1]);
	printf(" > %-3d : %-5u, %-.2f\n", ARRAY_SIZE(deviation_table) - 1,
		nr, (float)nr/NR_TESTS);
}
	return 0;
}

