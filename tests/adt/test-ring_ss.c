#include <skp/adt/ring.h>
#include <skp/process/thread.h>

#define TEST_NR (1U<<25)

static int enqueue_cb(void *_r)
{
	struct ringb *r = _r;
	cycles_t start = get_cycles();
	for (int i = 0; i < TEST_NR; i++) {
		void *ptr = (uintptr_t)i;
		do {
			int nr = ringb_enqueue(r, ptr);
			if (skp_likely(nr))
				break;
			cpu_relax();
		} while (1);
	}

	cycles_t escape = escape_cycles(start);

	log_info("enqueue cost : %llu ns", cycles_to_ns(escape)/TEST_NR);

	return 0;
}

static int dequeue_cb(void *_r)
{
	struct ringb *r = _r;

	cycles_t start = get_cycles();

	for (int i = 0; i < TEST_NR; i++) {
		void *ptr;
		do {
			int nr = ringb_dequeue(r, &ptr);
			if (skp_likely(nr)) {
				BUG_ON((uintptr_t)ptr!=i);
				break;
			}
			cpu_relax();
		} while (1);
	}

	cycles_t escape = escape_cycles(start);

	log_info("dequeue cost : %llu ns", cycles_to_ns(escape)/TEST_NR);

	return 0;
}

int main(void)
{
	log_info("start test ...");

	struct ringb *r;
	r = ringb_create(127, RINGB_F_EXACT_SZ|RINGB_F_SC_DEQ|RINGB_F_SP_ENQ);
	BUG_ON(!r);

	uthread_t tid[2];

	tid[0] = uthread_create(dequeue_cb, r);
	BUG_ON(!tid[0]);

	tid[1] = uthread_create(enqueue_cb, r);
	BUG_ON(!tid[1]);

	for (int i = 0; i < 2; i++) {
		uthread_wakeup(tid[i]);
	}

	for (int i = 0; i < 2; i++) {
		uthread_stop(tid[i], NULL);
	}

	log_info("finish test ...");

	BUG_ON(!ringb_empty(r));

	ringb_free(r);

	return EXIT_SUCCESS;
}

