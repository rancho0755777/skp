#include "test.h"
#include <skp/utils/utils.h>
#include <skp/utils/rwsem.h>

static bool wait_pthd = true;
static int nr_test = 1U << 25;
static int value = 0;
static DEFINE_RWSEM(lock);

static void *writelock_test(void *arg)
{
	thread_bind(-1);
	__cond_load_acquire(&wait_pthd, !VAL);

	uint64_t start = similar_abstime(0, 0);
	for (int  i = 0; i < nr_test; i++) {
		down_write(&lock);
		value++;
		up_write(&lock);
		//sched_yield();
	}

	uint64_t end = similar_abstime(0, 0);
	log_info("write cost : %llu", (int64_t)(end - start)/nr_test);
	return NULL;	
}

static void *readlock_test(void *arg)
{
	volatile int v;
	thread_bind(-1);
	__cond_load_acquire(&wait_pthd, !VAL);

	uint64_t start = similar_abstime(0, 0);
	for (int  i = 0; i < nr_test; i++) {
		down_read(&lock);
		v = READ_ONCE(value);
		//sched_yield();
		up_read(&lock);
	}

	uint64_t end = similar_abstime(0, 0);
	log_info("read cost : %llu", (int64_t)(end - start)/nr_test);
	return NULL;	
}

int main(void)
{
	pthread_t wpthd[1];
	pthread_t rpthd[3];
	
	for (int i = 0; i < ARRAY_SIZE(wpthd); i++) {
		wpthd[i] = thread_create(writelock_test, NULL);
	}
	for (int i = 0; i < ARRAY_SIZE(rpthd); i++) {
		rpthd[i] = thread_create(readlock_test, NULL);
	}

	WRITE_ONCE(wait_pthd, false);
	for (int i = 0; i < ARRAY_SIZE(rpthd); i++) {
		thread_join(rpthd[i]);
	}

	for (int i = 0; i < ARRAY_SIZE(wpthd); i++) {
		thread_join(wpthd[i]);
	}

	BUG_ON(value != nr_test * ARRAY_SIZE(wpthd));

	return 0;
}

