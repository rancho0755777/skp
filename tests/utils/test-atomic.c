#include "test.h"
#include <skp/utils/utils.h>
#include <skp/utils/atomic.h>

#ifndef __x86_64__
#define nr_pthd 4 
#else
#define nr_pthd (8)
#endif
#define nr_test (1U << 23)
static bool wait_pthd = true;

static uint8_t cmpxchg_val_8 = 0;
static uint16_t cmpxchg_val_16 = 0;
static uint32_t cmpxchg_val_32 = 0;
static uint64_t cmpxchg_val_64 = 0;

static atomic_t addv = ATOMIC_INIT(0);
static atomic_t subv = ATOMIC_INIT(nr_pthd * nr_test);
static atomic_t incv = ATOMIC_INIT(0);
static atomic_t decv = ATOMIC_INIT(nr_pthd * nr_test);
static atomic_t xaddv = ATOMIC_INIT(0);

static void *atomic_test(void *arg)
{
	thread_bind(-1);
	__cond_load_acquire(&wait_pthd, !VAL);

	cmpxchg8(&cmpxchg_val_8, 0, 1);
	cmpxchg16(&cmpxchg_val_16, 0, 1);
	cmpxchg32(&cmpxchg_val_32, 0, 1);
	cmpxchg64(&cmpxchg_val_64, 0, 1);

	for (int i = 0; i < nr_test; i++) {
		atomic_add(1, &addv);
		atomic_sub(1, &subv);
		atomic_inc(&incv);
		atomic_dec(&decv);
		atomic_inc_return(&xaddv);
	}

	return NULL;
}

static atomic64_t add64v = ATOMIC_INIT(0);
static atomic64_t sub64v = ATOMIC_INIT(nr_pthd * nr_test);
static atomic64_t inc64v = ATOMIC_INIT(0);
static atomic64_t dec64v = ATOMIC_INIT(nr_pthd * nr_test);
static atomic64_t xadd64v = ATOMIC_INIT(0);

static void *atomic64_test(void *arg)
{
	thread_bind(-1);
	__cond_load_acquire(&wait_pthd, !VAL);

	for (int i = 0; i < nr_test; i++) {
		atomic64_add(1, &add64v);
		atomic64_sub(1, &sub64v);
		atomic64_inc(&inc64v);
		atomic64_dec(&dec64v);
		atomic64_inc_return(&xadd64v);
	}

	return NULL;
}

static uint32_t SEQ = (1U << 30);
#define TEST_START() 								\
do {												\
	cycles_t _end;									\
	volatile long X = 0;							\
	cycles_t _start = get_cycles();					\
	for (volatile uint32_t i = 0; i < SEQ; i++) { 	\

#define TEST_END(name) 								\
	}												\
	_end = escape_cycles(_start);					\
	printf("[%s] cost : %.3lf ns\n", name, 			\
		(double)(cycles_to_ns(_end)) / SEQ); 		\
} while (0)

int main(int argc, char **argv)
{
	if (argc > 1) {
		SEQ = atoi(argv[1]);
	}

	TEST_START()
	X++;
	TEST_END("inc");

	TEST_START()
		xadd(&X, 1);
	TEST_END("lock xadd");

	TEST_START()
		__sync_add_and_fetch(&X, 1);
	TEST_END("lock inc and fetch");

	TEST_START()
		__sync_fetch_and_add(&X, 1);
	TEST_END("fetch and lock inc ");

	atomic_t a = ATOMIC_INIT(-1);
	BUG_ON(!atomic_inc_and_test(&a));

	pthread_t pthd32[nr_pthd];
	for (int i = 0; i < ARRAY_SIZE(pthd32); i++) {
		pthd32[i] = thread_create(atomic_test, NULL);
	}

	pthread_t pthd64[nr_pthd];
	for (int i = 0; i < ARRAY_SIZE(pthd64); i++) {
		pthd64[i] = thread_create(atomic64_test, NULL);
	}

	*(volatile bool*)&wait_pthd = false;

	for (int i = 0; i < ARRAY_SIZE(pthd64); i++) {
		thread_join(pthd64[i]);
	}
	BUG_ON(atomic64_read(&add64v) != nr_test * ARRAY_SIZE(pthd64));
	BUG_ON(atomic64_read(&sub64v) != 0);
	BUG_ON(atomic64_read(&inc64v) != nr_test * ARRAY_SIZE(pthd64));
	BUG_ON(atomic64_read(&dec64v) != 0);
	BUG_ON(atomic64_read(&xadd64v) != nr_test * ARRAY_SIZE(pthd64));

	for (int i = 0; i < ARRAY_SIZE(pthd32); i++) {
		thread_join(pthd32[i]);
	}

	BUG_ON(atomic_read(&addv) != nr_test * ARRAY_SIZE(pthd32));
	BUG_ON(atomic_read(&subv) != 0);
	BUG_ON(atomic_read(&incv) != nr_test * ARRAY_SIZE(pthd32));
	BUG_ON(atomic_read(&decv) != 0);
	BUG_ON(atomic_read(&xaddv) != nr_test * ARRAY_SIZE(pthd32));

	BUG_ON(cmpxchg_val_8!=1);
	BUG_ON(cmpxchg_val_16!=1);
	BUG_ON(cmpxchg_val_32!=1);
	BUG_ON(cmpxchg_val_64!=1);
	
	return 0;
}
