#include <skp/process/thread.h>
#include <skp/process/wait.h>

/*
 * 典型的 生产者和消费者 同步测试
 * AB互为生产者和消费者
 */

static int count_A = 0;
static int count_B = 0;

static int cond_A = 0;
static int cond_B = 0;

static DEFINE_WAIT_QUEUE_HEAD(wq_A);
static DEFINE_WAIT_QUEUE_HEAD(wq_B);

static int partner_A(void *arg)
{
	DEFINE_AUTOREMOVE_WAITQUEUE(wait);

	do {
		printf("wake up B\n");
		WRITE_ONCE(cond_B, 1);
		wake_up_one(&wq_B);

		printf("wait B\n");
		do {
			prepare_to_wait(&wq_A, &wait);
			if (READ_ONCE(cond_A))
				break;
			wait_on(&wait);
		} while (1);
		finish_wait(&wq_A, &wait);
		WRITE_ONCE(cond_A, 0);

		//usleep(1000);
		printf("exec A\n");

		if (WARN_ON(++count_A != READ_ONCE(count_B))) {
			printf("A : %d , B : %d\n", count_A, count_B);
			break;
		}

	} while (1);

	return 0;
}

static int partner_B(void *arg)
{
	DEFINE_AUTOREMOVE_WAITQUEUE(wait);

	do {
		printf("wait A\n");
		do {
			prepare_to_wait(&wq_B, &wait);
			if (READ_ONCE(cond_B))
				break;
			wait_on(&wait);
		} while(1);
		finish_wait(&wq_B, &wait);
		WRITE_ONCE(cond_B, 0);

		printf("exec B\n");

		if(WARN_ON(count_B++ != READ_ONCE(count_A))) {
			printf("A : %d , B : %d\n", count_A, count_B);
			break;
		}

		WRITE_ONCE(cond_A, 1);
		printf("wake up A\n");
		wake_up_one(&wq_A);
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
