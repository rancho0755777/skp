#include <skp/process/thread.h>
#include <skp/process/wait.h>

/*
 * 典型的 生产者和消费者 同步测试
 * 在不同的线程中进行加锁和解锁
 */

static int count_A = 0;
static int count_B = 0;

static int bit_A = 0;
static int bit_B = 1;
static unsigned long cond_bit_map = -1U;

static int sleep_on(wait_queue_t *wait)
{
	/*
	wait_bit_queue_t *wait_bit =
		wait_bit_queue_from_wait(wait);
	必须在等待队列上 等待，否则无法唤醒*/
	wait_on(wait);
	return 0;
}

static inline void clear_and_wakeup_bit(unsigned long *bit_map, int bit_idx)
{
	clear_bit(bit_idx, bit_map);
	static_mb();
	wake_up_bit(bit_map, bit_idx);
}

static int partner_A(void *arg)
{
	DEFINE_WAITBIT(wait, &cond_bit_map, bit_A);

	do {
		printf("wake up B\n");
		/*对B解锁*/
		clear_and_wakeup_bit(&cond_bit_map, bit_B);
		printf("wait B\n");
		wait_on_bit_lock(&cond_bit_map, bit_A, sleep_on);

		printf("exec A\n");

		if (WARN_ON(++count_A != READ_ONCE(count_B))) {
			log_warn("A : A : %d , B : %d", count_A, count_B);
			/*杀死 B 线程，否则无法推出，这里假设测试无误*/
			break;
		}

	} while (1);

	return 0;
}

static int partner_B(void *arg)
{
	DEFINE_WAITBIT(wait, &cond_bit_map, bit_B);

	do {
		printf("wait A\n");
		/*对B加锁*/
		wait_on_bit_lock(&cond_bit_map, bit_B, sleep_on);

		printf("exec B\n");

		if(WARN_ON(count_B++ != READ_ONCE(count_A))) {
			log_warn("B : A : %d , B : %d", count_A, count_B);
			break;
		}

		printf("wake up A\n");
		clear_and_wakeup_bit(&cond_bit_map, bit_A);
	} while(1);

	return 0;
}

int main(int argc, char const *argv[])
{
	uthread_t A, B;

	A = uthread_create(partner_A, 0);
	B = uthread_create(partner_B, 0);

	uthread_wakeup(B);
	uthread_wakeup(A);

	uthread_stop(A, 0);
	uthread_stop(B, 0);

	return 0;
}
