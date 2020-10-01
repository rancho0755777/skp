#include <skp/adt/idr.h>
#include <skp/process/thread.h>
#include <skp/process/signal.h>

static bool stopped = false;
static struct idt idt;

#ifndef START_ID
# define START_ID 0
#endif

#ifndef END_ID
# define END_ID ((1 << 20) - 1)
#endif

static void hdl_sig(int signo)
{
	stopped = true;
}

static int alloc_thread(void*_)
{
	long id;

	do {
		id = idt_alloc(&idt);
		if (WARN_ON(id < 0)) {
			sched_yield();
			continue;
		}
		if (skp_unlikely(id < START_ID || id > END_ID)) {
			log_warn("error id : %lld", id);
			BUG();
		}
	} while (!READ_ONCE(stopped));

	return 0;
}

static int free_thread(void*_)
{
	long id;

	do {
		id = prandom_int(START_ID, END_ID);
		idt_remove(&idt, id);
	} while (!READ_ONCE(stopped));

	return 0;
}

int main(int argc, char const *argv[])
{
	uint64_t start, end;
	int rc = idt_init(&idt, START_ID, END_ID);

	if (skp_unlikely(rc)) {
		log_error("init failed : %s", __strerror_local(rc));
		return EXIT_FAILURE;
	}

	start = abstime(0, 0);
	for (uint32_t i = 0; i < idt.nr_bit; i++) {
		long id = idt_alloc(&idt);
		BUG_ON(id < START_ID || id > END_ID);
	}
	end = abstime(0, 0);
	printf("alloc cost : %.3lf\n", (double)(end - start)/idt.nr_bit);

	BUG_ON(idt_alloc(&idt) > -1);

	start = abstime(0, 0);
	for (uint32_t i = 0; i < idt.nr_bit; i++) {
		idt_remove(&idt, i + START_ID);
	}
	end = abstime(0, 0);
	printf("remove cost : %.3lf\n", (double)(end - start)/idt.nr_bit);

	for (int i = 0; i < 20; i++) {
		int id = idt_alloc(&idt);
		log_debug("id : %d", id);
	}

	for (int i = 0; i < 10; i++) {
		idt_ring_remove(&idt, i);
	}

	for (int i = 0; i < 10; i++) {
		int id = idt_alloc(&idt);
		log_debug("id : %d", id);
	}

	signal_setup(SIGINT, hdl_sig);

	uthread_t allocthread[4];
	uthread_t freethread[4];

	signal_block_all(0);
	for (int i = 0; i < ARRAY_SIZE(allocthread); i++) {
		allocthread[i] = uthread_create(alloc_thread, 0);
	}
	for (int i = 0; i < ARRAY_SIZE(freethread); i++) {
		freethread[i] = uthread_create(free_thread, 0);
	}
	signal_unblock_all(0);

	for (int i = 0; i < ARRAY_SIZE(allocthread); i++) {
		uthread_wakeup(allocthread[i]);
	}

	for (int i = 0; i < ARRAY_SIZE(freethread); i++) {
		uthread_wakeup(freethread[i]);
	}

	sleep(10);
	stopped = true;

	for (int i = 0; i < ARRAY_SIZE(allocthread); i++) {
		uthread_stop(allocthread[i], 0);
	}

	for (int i = 0; i < ARRAY_SIZE(freethread); i++) {
		uthread_stop(freethread[i], 0);
	}

	idt_destroy(&idt);
	return 0;
}
