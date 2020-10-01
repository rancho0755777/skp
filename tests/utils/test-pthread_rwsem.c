#include "test.h"
#include <skp/utils/utils.h>
#include <skp/utils/mutex.h>

static bool wait_pthd = true;
static int nr_test = 1U << 25;
static int value = 0;
static pthread_rwlock_t lock = PTHREAD_RWLOCK_INITIALIZER;

static void *writelock_test(void *arg)
{
	thread_bind(-1);
	__cond_load_acquire(&wait_pthd, !VAL);

	uint64_t start = similar_abstime(0, 0);
	for (int  i = 0; i < nr_test; i++) {
		pthread_rwlock_wrlock(&lock);
		value++;
		pthread_rwlock_unlock(&lock);
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
		pthread_rwlock_rdlock(&lock);
		v = READ_ONCE(value);
		pthread_rwlock_unlock(&lock);
	}

	uint64_t end = similar_abstime(0, 0);
	log_info("read cost : %llu", (int64_t)(end - start)/nr_test);
	return NULL;	
}

int main(void)
{
	pthread_t wpthd[1];
	pthread_t rpthd[3];

 	pthread_rwlockattr_t attr;
    pthread_rwlockattr_init(&attr);
    pthread_rwlockattr_setkind_np(&attr, PTHREAD_RWLOCK_PREFER_WRITER_NP);
    //pthread_rwlockattr_setkind_np(&attr, PTHREAD_RWLOCK_PREFER_READER_NP);
    pthread_rwlock_init(&lock, &attr);
    pthread_rwlockattr_destroy(&attr);

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

	pthread_rwlock_destroy(&lock);

	return 0;
}