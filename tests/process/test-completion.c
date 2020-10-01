#include <skp/process/thread.h>
#include <skp/process/completion.h>

static int count_A = 0;
static int count_B = 0;

static DEFINE_COMPLETION(complete_A);
static DEFINE_COMPLETION(complete_B);

static int partner_A(void *arg)
{
	do {
		printf("wake up B\n");
		/*对B解锁*/
		complete(&complete_B);

		printf("wait B\n");
		wait_for_completion(&complete_A);

		//usleep(1000);
		printf("exec A\n");

		if (WARN_ON(++count_A != READ_ONCE(count_B))) {
			log_warn("A : A : %d , B : %d", count_A, count_B);
			/*杀死 B 线程，否则可能无法推出，这里假设测试无误*/
			break;
		}

	} while (1);

	return 0;
}

static int partner_B(void *arg)
{
	do {
		printf("wait A\n");
		/*对B加锁*/
		wait_for_completion(&complete_B);

		//usleep(1000);
		printf("exec B\n");

		if(WARN_ON(count_B++ != READ_ONCE(count_A))) {
			log_warn("B : A : %d , B : %d", count_A, count_B);
			break;
		}

		printf("wake up A\n");
		complete(&complete_A);
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
