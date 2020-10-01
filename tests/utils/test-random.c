#include <skp/utils/utils.h>
#include <skp/utils/atomic.h>
#include <skp/process/thread.h>

#define SEQ (1U << 24)

volatile static bool __wait = false;

static int thread_cb(void *args)
{
	uint64_t start, end;
	volatile long x = 0;
	uint32_t ivalues[100];
	double fvalues[100];

	while (!READ_ONCE(__wait))
		cpu_relax();

	for (int i = 0; i < 100; i++) {
		ivalues[i] = prandom_int(0, 100);
	}

	for (int i = 0; i < 100; i++) {
		fvalues[i] = prandom_real(0, 1);
	}

	big_lock();
	printf(">>> INT prandom : \n");
	for (int i = 1; i <= 100; i++) {
		printf("%03ld,", ivalues[i - 1]);
		if (! (i % 10) )
			printf("\n");
	}
	printf("\n");

	printf(">>> FLOAT prandom : \n");
	for (int i = 1; i <= 100; i++) {
		printf("%.5lf,", fvalues[i - 1]);
		if (! (i % 10) )
			printf("\n");
	}
	printf("\n");
	big_unlock();

	srandom(time(0));
	x = 0;
	start = abstime(0, 0);
	for (int i = 0; i < SEQ; i++) {
		x += random();
	}
	end = abstime(0, 0);
	log_info("(%ld)random cost : %lu", x, (long)(end - start) / SEQ);

	x = 0;
	start = abstime(0, 0);
	for (int i = 0; i < SEQ; i++) {
		x += prandom();
	}
	end = abstime(0, 0);
	log_info("(%ld)prandom cost : %lu", x, (long)(end - start) / SEQ);

	return 0;
}

int main(int argc, char const *argv[])
{
	uthread_t pthd[4];

	for (int i = 0; i < ARRAY_SIZE(pthd); i++) {
		pthd[i] = uthread_run(thread_cb, NULL);
		BUG_ON(!pthd[i]);
	}

	xchg(&__wait, true);

	for (int i = 0; i < ARRAY_SIZE(pthd); i++) {
		uthread_stop(pthd[i], NULL);
	}

	return 0;
}
