#include <sys/epoll.h>
#include <sys/signalfd.h>

#include <skp/mm/slab.h>

#ifndef __REACTOR_IMPL_MAGIC__
# error only <process/event.c> can be included directly
#endif

struct uev_looper;
struct uev_stream;
struct poll_event;

struct epoll_reactor {
	int epfd;
	int sigfd; /**< 信号监视器*/
	uint32_t poll_size;
	sigset_t sigmask; /**< 正在监控的信号*/
	sigset_t sigblock; /**< 注册时原来的阻塞状况*/
	struct epoll_event ready_events[0];
};

static inline void mask2epev(const struct poll_event *pe, struct epoll_event *ee)
{
	ee->events = 0;
	ee->data.u64 = pe->data;
	if (pe->mask & EVENT_READ) ee->events |= EPOLLIN;
	if (pe->mask & EVENT_WRITE) ee->events |= EPOLLOUT;
	if (pe->mask & EVENT_EDGE) ee->events |= EPOLLET;
}

static inline void epev2mask(const struct epoll_event *ee, struct poll_event *pe)
{
	pe->mask = 0;
	pe->data = ee->data.u64;
	if (ee->events & EPOLLOUT) pe->mask |= EVENT_WRITE;
	if (ee->events & (EPOLLIN | EPOLLPRI)) pe->mask |= EVENT_READ;
	if (ee->events & (EPOLLERR|EPOLLHUP)) pe->mask |= EVENT_ERROR;
	/*描述符被关闭*/
	if (skp_unlikely(ee->events & ~(EPOLLIN | EPOLLPRI | EPOLLOUT | EPOLLERR)))
		pe->mask |= EVENT_EOF;
}

#define __define_epoll_ctl(name, op)										\
static int name##_event(struct uev_looper *looper,							\
		struct uev_stream *ue, struct poll_event *pe)						\
{ 																			\
	int rc; 																\
	struct epoll_event ee;													\
	struct epoll_reactor * reactor = looper->reactor;						\
	mask2epev(pe, &ee);														\
	rc = epoll_ctl(reactor->epfd, EPOLL_CTL_##op, ue->fd, &ee);				\
	if (skp_unlikely(rc < 0)) { 											\
		log_warn("epoll_ctl/%s failed : %s", #op, strerror_local());		\
		return -errno; 														\
	}																		\
	return 0; 																\
}

__define_epoll_ctl(register, ADD)
__define_epoll_ctl(modify, MOD)
__define_epoll_ctl(unregister, DEL)

/*错误返回非零0*/
static int reactor_create(struct uev_looper *looper)
{
	int rc, epfd, sigfd;
	struct epoll_event ee;
	struct epoll_reactor *reactor;

	reactor = malloc(sizeof(*reactor) + sizeof(reactor->ready_events[0]) *
					looper->poll_size);
	if (skp_unlikely(!reactor))
		return -ENOMEM;

	reactor->epfd = -1;
	reactor->sigfd = -1;
	sigemptyset(&reactor->sigmask);
	sigemptyset(&reactor->sigblock);
	reactor->poll_size = looper->poll_size;

	epfd = epoll_create1(EPOLL_CLOEXEC);
	if (skp_unlikely(epfd < 0))
		goto fail_epfd;

	reactor->epfd = epfd;
	looper->reactor = reactor;
	if (!looper->siginfo)
		return 0;

	/*启动时将监控的信号集设置为空*/	
	sigfd = signalfd(-1, &reactor->sigmask, SFD_NONBLOCK|SFD_CLOEXEC);	
	if (skp_unlikely(sigfd < 0))
		goto fail_sigfd;

	ee.events = EPOLLIN;
	ee.data.u64 = pollevent_make_data(EV_SIGID, sigfd);
	rc = epoll_ctl(epfd, EPOLL_CTL_ADD, sigfd, &ee);
	if (skp_unlikely(rc < 0))
		goto fail_reg;

	reactor->sigfd = sigfd;
	return 0;

fail_reg:
	close(sigfd);
fail_sigfd:
	close(epfd);
fail_epfd:
	free(reactor);
	looper->reactor = NULL;
	return -EINVAL;
}

/*错误返回非零0*/
static int disable_event(struct uev_looper *looper,
		struct uev_stream *event, struct poll_event *pevent)
{
	/*epoll 支持边沿触发，不需要调整*/
	return 0;
}

static int enable_event(struct uev_looper *looper,
		struct uev_stream *event, struct poll_event *pevent)
{
	/*epoll 支持边沿触发，不需要调整*/
	return 0;
}

/*销毁不同实现的私有数据*/
static void reactor_destroy(struct uev_looper *looper)
{
	struct epoll_reactor * reactor = looper->reactor;
	if (skp_likely(reactor)) {
		if (looper->siginfo)
			close(reactor->sigfd);
		close(reactor->epfd);
		free(reactor);
	}
}

/*每次仅最多处理额定个数的信号*/
#define READ_NR_SIGINFO 4

/*返回就绪个数*/
static int reactor_poll(struct uev_looper *looper, int timeout)
{
	int nr;
	ssize_t n;
	struct poll_event *pe;
	struct uev_siginfo *siginfo;
	struct epoll_reactor *reactor = looper->reactor;

	siginfo = looper->siginfo;
	if (siginfo)
		bitmap_zero(siginfo->active, NSIG);

	nr = epoll_wait(reactor->epfd, &reactor->ready_events[0],
			reactor->poll_size, timeout);

	if (skp_unlikely(nr < 0)) {
		if (skp_likely(errno == EINTR))
			return 0;
		log_warn("epoll_wait failed : %s", strerror_local());
		return -errno;
	}

	EVENT_BUG_ON(nr > looper->poll_size);

	return nr;
}

static int pollevent_pull(struct uev_looper *looper, int idx,
		struct pool_event *pe)
{
	struct epoll_event *ee;
	struct uev_siginfo *siginfo = looper->siginfo;
	struct epoll_reactor *reactor = looper->reactor;


	ee = &reactor->ready_events[idx];
	EVENT_BUG_ON(idx >= looper->poll_size);

	epev2mask(ee, pe);
	if (skp_unlikely(pollevent_fd(pe) == reactor->sigfd)) {
		ssize_t n;
		struct signalfd_siginfo fdsi[READ_NR_SIGINFO];
		/*收集触发的信号*/
		EVENT_BUG_ON(!siginfo);
		EVENT_BUG_ON(pollevent_id(pe) != EV_SIGID);
		n = read(reactor->sigfd, fdsi, sizeof(fdsi));
		EVENT_BUG_ON(n > sizeof(fdsi));
		if (skp_unlikely((n < 0 || (n % sizeof(fdsi[0]))) && errno != EAGAIN)) {
			log_error("read sigfd failed : %s", strerror_local());
			return n < 0 ? -errno : -ENODEV;
		}
		n /= sizeof(fdsi[0]);	
		for (ssize_t i = 0; i < n; i++) {
			__set_bit(fdsi[i].ssi_signo, siginfo->active);
		}
		return -EAGAIN;
	}
	return 0;
}

static bool epoll_signal_saved(struct epoll_reactor *reactor, int signo)
{
	return !!sigismember(&reactor->sigmask, signo);
}

static bool epoll_signal_save(struct epoll_reactor *reactor, int signo)
{
	sigset_t mask, old;

	/*已经被监控*/
	if (epoll_signal_saved(reactor, signo))
		return false;

	/*必须全进程阻塞信号*/
	sigemptyset(&mask);
	sigaddset(&mask, signo);
	sigprocmask(SIG_BLOCK, &mask, &old);
	/*原来就被阻塞了，记录这个事实以便恢复*/
	if (sigismember(&old, signo))
		sigaddset(&reactor->sigblock, signo);
	sigaddset(&reactor->sigmask, signo);
	return true;
}

static bool epoll_signal_recover(struct epoll_reactor *reactor, int signo)
{
	sigset_t mask;

	/*已经被监控*/
	if (!epoll_signal_saved(reactor, signo))
		return false;

	sigemptyset(&mask);
	sigaddset(&mask, signo);
	/*不在阻塞集合中，则表示是事件API阻塞的，需要恢复*/
	if (!sigismember(&reactor->sigblock, signo))
		sigprocmask(SIG_UNBLOCK, &mask, 0);
	sigdelset(&reactor->sigmask, signo);
	sigdelset(&reactor->sigblock, signo);
	return true;
}

static int register_signal(struct uev_looper *looper, int signo)
{
	int rc;
	struct epoll_reactor * reactor = looper->reactor;
	EVENT_BUG_ON(!looper->siginfo);

	if (!epoll_signal_save(reactor, signo))
		return 0;

	rc = signalfd(reactor->sigfd, &reactor->sigmask, 0);
	if (skp_unlikely(rc < 0)) {
		log_warn("signalfd/register signal [%d] failed : %s",
			signo, strerror_local());
		epoll_signal_recover(reactor, signo);
		return -errno;
	}
	EVENT_BUG_ON(rc != reactor->sigfd);	
	return 0;
}

static int unregister_signal(struct uev_looper *looper, int signo)
{
	int rc;
	struct epoll_reactor * reactor = looper->reactor;

	EVENT_BUG_ON(!looper->siginfo);

	if (!epoll_signal_recover(reactor, signo))
		return -EINVAL;

	rc = signalfd(reactor->sigfd, &reactor->sigmask, 0);
	if (skp_unlikely(rc < 0)) {
		log_warn("signalfd/unregister signal [%d] failed : %s",
			signo, strerror_local());
		return -errno;
	}
	EVENT_BUG_ON(rc != reactor->sigfd);	

	return 0;
}
