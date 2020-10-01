#include <skp/utils/utils.h>
#include <skp/utils/spinlock.h>
#include <skp/utils/rwlock.h>
#include <skp/utils/mutex.h>
#include <skp/utils/rwsem.h>

#include <pthread.h>

static uint32_t SEQ = (1U << 24);

#define TEST_START() \
do {							\
	uint64_t _start, _end;		\
	volatile long X = 0;	\
	_start = abstime(0, 0); \
	for (volatile uint32_t i = 0; i < SEQ; i++) { \

#define TEST_FINISH(name) \
	}							\
	_end = abstime(0, 0);	\
	printf("[%s] cost : %lu ns\n", name, (long)(_end - _start) / SEQ); \
} while (0)

int main(int argc, char const *argv[])
{
	DEFINE_SPINLOCK(spin);
	DEFINE_RWLOCK(rw);
	DEFINE_MUTEX(mutex);
	DEFINE_RWSEM(sem);
	pthread_mutex_t pmutex = PTHREAD_MUTEX_INITIALIZER;
#ifdef __linux__
	pthread_spinlock_t pspin;
	pthread_spin_init(&pspin, PTHREAD_PROCESS_PRIVATE);
#endif

	if (argc > 1) {
		SEQ = atoi(argv[1]);
	}

/*benchmark*/
	TEST_START()
		X += prandom_int(1, 100);
	TEST_FINISH("base");

/*spinlock*/
	TEST_START()
		spin_lock(&spin);
		X += prandom_int(1, 100);
		spin_unlock(&spin);
	TEST_FINISH("spinlock");

#ifdef __linux__
	TEST_START()
		pthread_spin_lock(&pspin);
		X += prandom_int(1, 100);
		pthread_spin_unlock(&pspin);
	TEST_FINISH("pthread spinlock");
#endif

	TEST_START()
		read_lock(&rw);
		X += prandom_int(1, 100);
		read_unlock(&rw);
	TEST_FINISH("read lock");

	TEST_START()
		write_lock(&rw);
		X += prandom_int(1, 100);
		write_unlock(&rw);
	TEST_FINISH("write lock");

	TEST_START()
		mutex_lock(&mutex);
		X += prandom_int(1, 100);
		mutex_unlock(&mutex);
	TEST_FINISH("mutex");

	TEST_START()
		pthread_mutex_lock(&pmutex);
		X += prandom_int(1, 100);
		pthread_mutex_unlock(&pmutex);
	TEST_FINISH("pthread mutex");

	TEST_START()
		down_read(&sem);
		X += prandom_int(1, 100);
		up_read(&sem);
	TEST_FINISH("read sem");

	TEST_START()
		down_write(&sem);
		X += prandom_int(1, 100);
		up_write(&sem);
	TEST_FINISH("write sem");
	
	return 0;
}
