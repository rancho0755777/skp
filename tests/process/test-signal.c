#include <pthread.h>
#include <skp/process/signal.h>
#include <skp/process/event.h>
#include <skp/process/completion.h>

DEFINE_COMPLETION(quit_cmpl);

static void signal_handle(struct uev_signal *signal)
{
	log_info("interrupte by %d", signal->signo);
	if (signal->signo == SIGQUIT)
		complete(&quit_cmpl);
}

static int nr_async = 0;
static void async_handle(struct uev_async *async)
{
	nr_async++;
	WARN_ON(nr_async != 1);
}

int main(void)
{
	int rc;
	struct uev_async async;
	struct uev_signal signal_init;
	struct uev_signal signal_quit;

	uev_async_init(&async, async_handle);
	uev_signal_init(&signal_init, SIGINT, signal_handle);
	uev_signal_init(&signal_quit, SIGQUIT, signal_handle);

	rc = uev_async_register(&async);
	BUG_ON(rc);
	rc = uev_signal_register(&signal_init);
	BUG_ON(rc);
	rc = uev_signal_register(&signal_quit);
	BUG_ON(rc);

	rc = uev_async_emit(&async);
	BUG_ON(rc);
	sched_yield();
	rc = uev_async_emit(&async);
	BUG_ON(rc);

	wait_for_completion(&quit_cmpl);

	uev_signal_unregister_sync(&signal_init);
	uev_signal_unregister_sync(&signal_quit);

	return EXIT_SUCCESS;
}
