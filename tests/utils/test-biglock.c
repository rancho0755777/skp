#include "test.h"
#include <skp/utils/utils.h>

static bool wait_pthd = true;
static int nr_test = 1U << 10; 
static int value = 0;

static void *biglock_test(void *arg)
{
	thread_bind(-1);
	__cond_load_acquire(&wait_pthd, !VAL);

	for (int  i = 0; i < nr_test; i++) {
		big_lock();
		big_lock();
		value++;
		big_unlock();
		big_unlock();
	}

	return NULL;	
}

int main(void)
{
	pthread_t pthd[4];
	
	for (int i = 0; i < ARRAY_SIZE(pthd); i++) {
		pthd[i] = thread_create(biglock_test, NULL);
	}

	WRITE_ONCE(wait_pthd, false);
	
	for (int i = 0; i < ARRAY_SIZE(pthd); i++) {
		thread_join(pthd[i]);
	}

	BUG_ON(value != nr_test * ARRAY_SIZE(pthd));

	return 0;
}

