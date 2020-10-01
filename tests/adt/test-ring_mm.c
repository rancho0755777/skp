#include <skp/adt/ring.h>
#include <skp/utils/mutex.h>
#include <skp/process/thread.h>

#define TEST_NR (1U<<25)
#define THREAD_NR (2)

//#define TEST_USE_SPINLOCK
//#define TEST_USE_MUTEX
//#define TEST_USE_PMUTEX

#ifdef TEST_USE_MUTEX
DEFINE_MUTEX(locker);
#define TEST_LK() mutex_lock(&locker)
#define TEST_ULK() mutex_unlock(&locker)
#elif defined(TEST_USE_SPINLOCK)
DEFINE_SPINLOCK(locker);
#define TEST_LK() spin_lock(&locker)
#define TEST_ULK() spin_unlock(&locker)
#elif defined(TEST_USE_PMUTEX)
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
#define TEST_LK() pthread_mutex_lock(&lock)
#define TEST_ULK() pthread_mutex_unlock(&lock)
#else
#define TEST_LK() ((void)0)
#define TEST_ULK() ((void)0)
#endif

static uint64_t base_cost = 0;

static int dequeue_cb(void *_r)
{
	struct ringb *r = _r;
	uint32_t *slot = r->user;
	BUG_ON(!slot);
	uint32_t *index = slot - 2;

	cycles_t start = get_cycles();

	for (int i = 0; i < TEST_NR/THREAD_NR; i++) {
		void *ptr;
		do {
			TEST_LK();
			int nr = ringb_dequeue(r, &ptr);
			TEST_ULK();
			if (skp_likely(nr)) {
				slot[xadd(index, 1)] = (uint32_t)ptr;
				break;
			}
			cpu_relax();
		} while (1);
	}

	cycles_t escape = escape_cycles(start);
	uint64_t cost = cycles_to_ns(escape)/(TEST_NR/THREAD_NR) - base_cost;

	log_info("dequeue cost : %llu ns", cost);

	return 0;
}

static int enqueue_cb(void *_r)
{
	struct ringb *r = _r;
	uint32_t *slot = r->user;
	BUG_ON(!slot);
	uint32_t *aval = slot - 1;

	cycles_t start = get_cycles();
	for (int i = 0; i < TEST_NR/THREAD_NR; i++) {
		void *ptr = (uintptr_t)xadd(aval, 1);
		do {
			TEST_LK();
			int nr = ringb_enqueue(r, ptr);
			TEST_ULK();
			if (skp_likely(nr))
				break;
			cpu_relax();
		} while (1);
	}

	cycles_t escape = escape_cycles(start);
	uint64_t cost = cycles_to_ns(escape)/(TEST_NR/THREAD_NR) - base_cost;
	log_info("enqueue cost : %llu ns", cost);

	return 0;
}

static int compare(const void *_a, const void *_b)
{
	uint32_t a = *(uint32_t*)_a;
	uint32_t b = *(uint32_t*)_b;
	return a < b?-1:(a==b?0:1);
}

int main(void)
{
	log_info("start test ...");

	cycles_t start = get_cycles();
	volatile int c = 0;
	volatile int j = 1U<<20;
	for (int i = 0; i<j; i++) {
		xadd(&c, 1);
	}
	cycles_t esc = escape_cycles(start);
	base_cost = cycles_to_ns(esc)/j;

	log_info("atomic op base cost : %llu", base_cost);

	struct ringb *r = ringb_create(127, RINGB_F_EXACT_SZ
#if defined(TEST_USE_MUTEX) || defined(TEST_USE_SPINLOCK) || defined(TEST_USE_PMUTEX)
		|RINGB_F_SC_DEQ|RINGB_F_SP_ENQ
#endif
	);
	BUG_ON(!r);

	uint32_t *slot = calloc(TEST_NR+2, sizeof(*slot));
	BUG_ON(!slot);
	r->user = slot+2;

	uthread_t tid[THREAD_NR*2];

	int i = 0;
	for (i=0; i<ARRAY_SIZE(tid)/2; i++) {
		tid[i]=uthread_create(dequeue_cb, r);
		BUG_ON(!tid[i]);
	}

	for (; i<ARRAY_SIZE(tid); i++) {
		tid[i]=uthread_create(enqueue_cb, r);
		BUG_ON(!tid[i]);
	}

	for (i=0; i<ARRAY_SIZE(tid); i++) {
		uthread_wakeup(tid[i]);
	}

	for (i=0; i<ARRAY_SIZE(tid); i++) {
		uthread_stop(tid[i], NULL);
	}

	log_info("finish test ...");

	BUG_ON(!ringb_empty(r));

	qsort(r->user, TEST_NR, sizeof(*slot), compare);

	for (int i = 0; i < TEST_NR; i++) {
		uint32_t *slot = r->user;
		BUG_ON(slot[i]!=i);
	}

	free(slot);
	ringb_free(r);

	return EXIT_SUCCESS;
}
