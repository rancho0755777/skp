#include <sys/epoll.h>
#include <sys/signalfd.h>

#include <skp/mm/slab.h>

#ifndef __REACTOR_IMPL_MAGIC__
# error only <process/event.c> can be included directly
#endif

struct uev_slot;
struct uev_stream;
struct poll_event;

struct epoll_reactor {
	int epfd;
	int sigfd; /**< 信号监视器*/
	uint32_t ready_size;
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
	if (ee->events & (EPOLLIN | EPOLLPRI)) pe->mask |= EVENT_READ;
	if (ee->events & EPOLLOUT) pe->mask |= EVENT_WRITE;
	if (ee->events & (EPOLLERR|EPOLLHUP)) pe->mask |= EVENT_ERROR;
	/*描述符被关闭*/
	if (skp_unlikely(ee->events & ~(EPOLLIN | EPOLLPRI | EPOLLOUT | EPOLLERR)))
		pe->mask |= EVENT_EOF;
}

#define __define_epoll_ctl(name, op)										\
static int reactor_##name##_event(struct uev_slot *slot,					\
		struct uev_stream *ue, struct poll_event *pe)						\
{ 																			\
	int rc; 																\
	struct epoll_event ee;													\
	struct epoll_reactor * reactor = slot->reactor;							\
	mask2epev(pe, &ee);														\
	rc = epoll_ctl(reactor->epfd, EPOLL_CTL_##op, ue->fd, &ee);				\
	if (skp_unlikely(rc < 0)) { 												\
		log_warn("epoll_ctl/%s failed : %s", #op, strerror_local());		\
		return -errno; 														\
	}																		\
	return 0; 																\
}

__define_epoll_ctl(register, ADD)
__define_epoll_ctl(modify, MOD)
__define_epoll_ctl(unregister, DEL)

/*错误返回非零0*/
static int reactor_create(struct uev_slot *slot)
{
	int rc, epfd, sigfd;
	struct epoll_event ee;
	struct epoll_reactor *reactor;

	reactor = malloc(sizeof(*reactor) + sizeof(reactor->ready_events[0]) *
					slot->ready_size);
	if (skp_unlikely(!reactor))
		return -ENOMEM;

	reactor->epfd = -1;
	reactor->sigfd = -1;
	sigemptyset(&reactor->sigmask);
	sigemptyset(&reactor->sigblock);
	reactor->ready_size = slot->ready_size;

	epfd = epoll_create1(EPOLL_CLOEXEC);
	if (skp_unlikely(epfd < 0))
		goto fail_epfd;

	reactor->epfd = epfd;
	slot->reactor = reactor;
	if (!slot->siginfo)
		return 0;

	/*启动时将监控的信号集设置为空*/	
	sigfd = signalfd(-1, &reactor->sigmask, SFD_NONBLOCK|SFD_CLOEXEC);	
	if (skp_unlikely(sigfd < 0))
		goto fail_sigfd;

	ee.events = EPOLLIN;
	ee.data.u64 = poll_mk_data(EV_SIGID, sigfd);
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
	slot->reactor = NULL;
	return -EINVAL;
}

/*错误返回非零0*/
static int reactor_disable_event(struct uev_slot *slot,
		struct uev_stream *event, struct poll_event *pevent)
{
	/*epoll 支持边沿触发，不需要调整*/
	return 0;
}

static int reactor_enable_event(struct uev_slot *slot,
		struct uev_stream *event, struct poll_event *pevent)
{
	/*epoll 支持边沿触发，不需要调整*/
	return 0;
}

/*销毁不同实现的私有数据*/
static void reactor_destroy(struct uev_slot *slot)
{
	struct epoll_reactor * reactor = slot->reactor;
	if (skp_likely(reactor)) {
		if (slot->siginfo)
			close(reactor->sigfd);
		close(reactor->epfd);
		free(reactor);
	}
}

/*每次仅最多处理额定个数的信号*/
#define READ_NR_SIGINFO 4

/*返回就绪个数*/
static int reactor_poll(struct uev_slot *slot, int timeout)
{
	int nr;
	ssize_t n;
	struct poll_event *pe;
	struct epoll_event *ee;
	struct uev_siginfo *siginfo;
	struct signalfd_siginfo fdsi[READ_NR_SIGINFO];
	struct epoll_reactor *reactor = slot->reactor;

	siginfo = slot->siginfo;
	if (siginfo)
		bitmap_zero(siginfo->active, NSIG);

	nr = epoll_wait(reactor->epfd, &reactor->ready_events[0],
			reactor->ready_size, timeout);

	if (skp_unlikely(nr < 0)) {
		if (skp_likely(errno == EINTR))
			return 0;
		log_warn("epoll_wait failed : %s", strerror_local());
		return -errno;
	}

	EVENT_BUG_ON(nr > slot->ready_size);

	for (int i = 0; i < nr; i++) {
		pe = &slot->ready_events[i];
		ee = &reactor->ready_events[i];
		epev2mask(ee, pe);
		if (skp_unlikely(poll_get_fd(pe) == reactor->sigfd)) {
			/*收集触发的信号*/
			EVENT_BUG_ON(!siginfo);
			EVENT_BUG_ON(poll_get_id(pe) != EV_SIGID);
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
		}
	}

	return nr;
}

static bool epoll_signal_saved(struct epoll_reactor *rct, int signo)
{
	return !!sigismember(&rct->sigmask, signo);
}

static bool epoll_signal_save(struct epoll_reactor *rct, int signo)
{
	sigset_t mask, old;

	/*已经被监控*/
	if (epoll_signal_saved(rct, signo))
		return false;

	/*必须全进程阻塞信号*/
	sigemptyset(&mask);
	sigaddset(&mask, signo);
	sigprocmask(SIG_BLOCK, &mask, &old);
	/*原来就被阻塞了，记录这个事实以便恢复*/
	if (sigismember(&old, signo))
		sigaddset(&rct->sigblock, signo);
	sigaddset(&rct->sigmask, signo);
	return true;
}

static bool epoll_signal_recover(struct epoll_reactor *rct, int signo)
{
	sigset_t mask;

	/*已经被监控*/
	if (!epoll_signal_saved(rct, signo))
		return false;

	sigemptyset(&mask);
	sigaddset(&mask, signo);
	/*不在阻塞集合中，则表示是事件API阻塞的，需要恢复*/
	if (!sigismember(&rct->sigblock, signo))
		sigprocmask(SIG_UNBLOCK, &mask, 0);
	sigdelset(&rct->sigmask, signo);
	sigdelset(&rct->sigblock, signo);
	return true;
}

static int reactor_register_signal(struct uev_slot *slot, int signo)
{
	int rc;
	struct epoll_reactor * reactor = slot->reactor;
	EVENT_BUG_ON(!slot->siginfo);

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

static int reactor_unregister_signal(struct uev_slot *slot, int signo)
{
	int rc;
	struct epoll_reactor * reactor = slot->reactor;

	EVENT_BUG_ON(!slot->siginfo);

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
