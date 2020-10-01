#include <skp/utils/utils.h>
#include <skp/process/thread.h>
#include <skp/process/completion.h>

static DEFINE_COMPLETION(detach_comp);

static int thread_normal_cb(void *arg)
{
	log_info("thread start [%d] : %s", get_thread_id(), (const char*)arg);
	thread_bind(-1);
	log_info("thread finish [%d] : %s", get_thread_id(), (const char*)arg);
	return 0;
}

static int thread_detach_cb(void *arg)
{
	log_info("thread start [%d] : %s", get_thread_id(), (const char*)arg);
	thread_bind(-1);
	uthread_detach();
	complete(&detach_comp);
	log_info("thread finish [%d] : %s", get_thread_id(), (const char*)arg);
	return 0;
}

static int nr_perf = 0;
static uint64_t total_cost = 0;

static int thread_perf_cb(void *arg)
{
	cycles_t start = *(cycles_t*)arg;
	cycles_t cost = get_cycles() - start;

	xadd(&total_cost, cost);
	xadd(&nr_perf, 1);

	free(arg);

	uthread_detach();
	return 0;
}

static void *thread_perf_wrap(void *arg)
{
	cycles_t start = *(cycles_t*)arg;
	cycles_t cost = get_cycles() - start;

	xadd(&total_cost, cost);
	xadd(&nr_perf, 1);

	free(arg);

	pthread_detach(pthread_self());
	pthread_exit(NULL);
}

static int tls_count = 4;
static void tls_cb(void *ptr)
{
	BUG_ON(ptr != (void*)1);
	xadd(&tls_count, -1);
}

static int thread_tls_cb(void *arg)
{
	tlsclnr_register(tls_cb, (void*)1);
	tlsclnr_register(tls_cb, (void*)1);
	return 0;
}

static void main_tls_cb(void)
{
	WARN_ON(tls_count != -1);
}

int main(int argc, char const *argv[])
{
	/*线程测试，基础测试*/
	uthread_t thread;

	atexit(main_tls_cb);

	thread = uthread_run(thread_detach_cb, "HelloWorld:Detach");
	wait_for_completion(&detach_comp);

	thread = uthread_run(thread_normal_cb, "HelloWorld:Join");
	if (skp_unlikely(!thread))
		log_error("create thread failed");

	uthread_stop(thread, NULL);

	/*创建性能测试*/
	int nr = 0;
	while (nr<20) {
		int rc;
		pthread_t tid;
		cycles_t *start = malloc(sizeof(*start));
		*start = get_cycles();
		rc = pthread_create(&tid, NULL, thread_perf_wrap, start);
		if (WARN_ON(rc))
			break;
		nr++;
	}

	while (READ_ONCE(nr_perf)!=nr) {
		msleep_unintr(500);
	}

	printf("create pthread cost : %lld us\n",
		cycles_to_ns(total_cost/nr)/1000);

	nr = 0;
	nr_perf = 0;
	total_cost = 0;

	while (nr<20) {
		cycles_t *start = malloc(sizeof(*start));
		*start = get_cycles();
		thread = uthread_run(thread_perf_cb, start);
		if (WARN_ON(!thread))
			break;
		nr++;
	}

	while (READ_ONCE(nr_perf)!=nr) {
		msleep_unintr(500);
	}

	printf("create uthread cost : %lld us\n",
		cycles_to_ns(total_cost/nr)/1000);

	/*线程私有数据测试*/
	uthread_t tls1, tls2;

	tls1 = uthread_run(thread_tls_cb, NULL);
	if (skp_unlikely(!tls1))
		log_error("create thread failed");

	tls2 = uthread_run(thread_tls_cb, NULL);
	if (skp_unlikely(!tls2))
		log_error("create thread failed");

	uthread_stop(tls1, NULL);
	uthread_stop(tls2, NULL);

	/*回调可能没有完成 需要等待一会才能验证*/
	sleep_unintr(1);
	BUG_ON(tls_count);

	tlsclnr_register(tls_cb, (void*)1);

	/*测试主线程 TLS 清理*/
	pthread_exit(NULL);
}
