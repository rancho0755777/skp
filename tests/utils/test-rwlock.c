#include "test.h"
#include <skp/utils/utils.h>

static bool wait_pthd = true;
static int nr_test = 1U << 23;
static int value = 0;

#if 0

typedef struct {
	volatile int32_t cnt; /**< -1 when W lock held, > 0 when R locks held. */
} rwlock_t;

#define __RWLOCK_INITIALIZER(x) { 0 }
#define DEFINE_RWLOCK(x) rwlock_t x = __RWLOCK_INITIALIZER(x)

static inline void rwlock_init(rwlock_t *rwl)
{
	rwl->cnt = 0;
}

static inline void read_lock(rwlock_t *rwl)
{
	int32_t x;
	int success = 0;

	while (success == 0) {
		x = rwl->cnt;
		/* write lock is held */
		if (x < 0) {
			cpu_relax();
			continue;
		}
		success = cmpxchg((volatile uint32_t *)&rwl->cnt,
					      x, x + 1);
	}
}

static inline void read_unlock(rwlock_t *rwl)
{
	xadd(&rwl->cnt, -1);
}

static inline void write_lock(rwlock_t *rwl)
{
	int32_t x;
	int success = 0;

	while (success == 0) {
		x = rwl->cnt;
		/* a lock is held */
		if (x != 0) {
			cpu_relax();
			continue;
		}
		success = cmpxchg((volatile uint32_t *)&rwl->cnt,
					      0, -1);
	}
}

static inline void write_unlock(rwlock_t *rwl)
{
	xadd(&rwl->cnt, 1);
}
#else
#include <skp/utils/rwlock.h>
#endif

static DEFINE_RWLOCK(lock);

static void *writelock_test(void *arg)
{
	thread_bind(-1);
	__cond_load_acquire(&wait_pthd, !VAL);

	uint64_t start = similar_abstime(0, 0);
	for (int  i = 0; i < nr_test; i++) {
		write_lock(&lock);
		value++;
		write_unlock(&lock);
	}

	uint64_t end = similar_abstime(0, 0);

	log_info("write cost : %llu", (uint64_t)(end - start)/nr_test);
	return NULL;	
}

static void *readlock_test(void *arg)
{
	volatile int v;
	thread_bind(-1);
	__cond_load_acquire(&wait_pthd, !VAL);

	uint64_t start = similar_abstime(0, 0);
	for (int  i = 0; i < nr_test; i++) {
		read_lock(&lock);
		v = READ_ONCE(value);
		read_unlock(&lock);
		sched_yield();
	}

	uint64_t end = similar_abstime(0, 0);

	log_info("read cost : %llu", (uint64_t)(end - start)/nr_test);
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

