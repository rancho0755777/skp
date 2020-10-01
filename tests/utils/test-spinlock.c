#include "test.h"
#include <skp/utils/utils.h>
#include <skp/utils/spinlock.h>

static bool wait_pthd = true;
static int nr_test = 1U << 25;
static int value = 0;
static DEFINE_SPINLOCK(lock);

static void *spinlock_test(void *arg)
{
	thread_bind(-1);
	__cond_load_acquire(&wait_pthd, !VAL);

	uint64_t start = similar_abstime(0, 0);

	for (int  i = 0; i < nr_test; i++) {
#if 0
		spin_fairlock(&lock);
#else
		spin_lock(&lock);
#endif
		value++;
		spin_unlock(&lock);
	}
	uint64_t end = similar_abstime(0, 0);

	log_info("cost : %llu", (uint64_t)(end - start)/nr_test);

	return NULL;	
}

int main(void)
{
	pthread_t pthd[4];

	log_info("start");
	
	for (int i = 0; i < ARRAY_SIZE(pthd); i++) {
		pthd[i] = thread_create(spinlock_test, NULL);
	}

	WRITE_ONCE(wait_pthd, false);
	
	for (int i = 0; i < ARRAY_SIZE(pthd); i++) {
		thread_join(pthd[i]);
	}

	BUG_ON(value != nr_test * ARRAY_SIZE(pthd));

	return 0;
}

