#include <pthread.h>
#include <skp/utils/utils.h>
#include <skp/process/signal.h>

static inline signal_fn __signal_setup(int signo, signal_fn cb, int flags)
{
	struct sigaction old, new;

	BUG_ON(signo == SIGKILL || signo == SIGSTOP);

	new.sa_handler = cb;
	new.sa_flags = flags;
	sigemptyset(&new.sa_mask);

	if (sigaction(signo, &new, &old))
		return SIG_ERR;

	return old.sa_handler;
}

signal_fn signal_setup(int signo, signal_fn cb)
{
	int flags = 0;
	if (signo == SIGALRM) {
#ifdef SA_INTERRUPT
		flags = SA_INTERRUPT;
#endif
	} else {
#ifdef SA_RESTART
		flags = SA_RESTART;
#endif
	}
	return __signal_setup(signo, cb, flags);
}

signal_fn signal_intr_setup(int signo, signal_fn cb)
{
	int flags = 0;
#ifdef SA_INTERRUPT
	flags = SA_INTERRUPT;
#endif
	return __signal_setup(signo, cb, flags);
}

void signal_block_all(sigset_t *old)
{
	int rc;
	sigset_t new;
	sigfillset(&new);
	rc = pthread_sigmask(SIG_BLOCK, &new, old);
	BUG_ON(rc);
}

void signal_unblock_all(const sigset_t *old)
{
	int rc;
	if (old) {
		rc = pthread_sigmask(SIG_SETMASK, old, NULL);
	} else {
		sigset_t new;
		sigfillset(&new);
		rc = pthread_sigmask(SIG_UNBLOCK, &new, NULL);
	}
	BUG_ON(rc);
}

int signal_block_one(int signo, sigset_t *old)
{
	int rc;
	sigset_t new;
	sigemptyset(&new);
	sigaddset(&new, signo);
	rc = pthread_sigmask(SIG_BLOCK, &new, old);
	if (rc) {
		errno = rc;
		return -1;
	}
	return 0;
}

int signal_unblock_one(int signo)
{
	int rc;
	sigset_t new;
	sigemptyset(&new);
	sigaddset(&new, signo);
	rc = pthread_sigmask(SIG_UNBLOCK, &new, NULL);
	if (rc) {
		errno = rc;
		return -1;
	}
	return 0;
}

void signal_default(int signo)
{
	log_info("Interrupt by %d : %s", signo, sys_siglist[signo]);
}
