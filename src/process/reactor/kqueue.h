//
//  kqueue.c
//
//  Created by 周凯 on 2018/11/14.
//  Copyright © 2018 zhoukai. All rights reserved.
//

#include <sys/event.h>
#include <skp/mm/slab.h>

#ifndef __REACTOR_IMPL_MAGIC__
# error only <process/event.c> can be included directly
#endif

struct uev_stream;
struct poll_event;
struct uev_slot;

struct kqueue_reactor {
	int kqfd;
	uint32_t ready_size;
	signal_fn signal_defhdl[NSIG];
	struct kevent64_s ready_events[0];
};

static inline void kqev2mask(const struct kevent64_s *ke, struct poll_event *pe)
{
	pe->mask = 0;
	pe->data = ke->udata;
	if (skp_unlikely(ke->filter == EVFILT_SIGNAL)) {
		pe->mask |= EVENT_R0;
		return;
	}
	if (ke->filter == EVFILT_READ) pe->mask |= EVENT_READ;
	if (ke->filter == EVFILT_WRITE) pe->mask |= EVENT_WRITE;
	if (ke->flags & EV_EOF) pe->mask |= EVENT_EOF;
	if (ke->flags & EV_ERROR) pe->mask |= EVENT_ERROR;
}

/*
 * 实现 reactor 接口
 */
#define __define_kqueue_ctl(name, op)								\
static int reactor_##name##_event(struct uev_slot *slot,			\
		struct uev_stream *ue, struct poll_event *pe)				\
{																	\
	int rc, nr = 0;													\
	struct kevent64_s ke[3]; 										\
	struct kqueue_reactor *reactor = slot->reactor;					\
	if (pe->mask & EVENT_READ) {									\
		EV_SET64(&ke[nr++], ue->fd, EVFILT_READ,					\
			EV_##op, 0, 0, pe->data, 0, 0);							\
	}																\
	if (pe->mask & EVENT_WRITE) {									\
		EV_SET64(&ke[nr++], ue->fd, EVFILT_WRITE,					\
			EV_##op, 0, 0, pe->data, 0, 0);							\
	}																\
	if (pe->mask & EVENT_ERROR)	{									\
		EV_SET64(&ke[nr++], ue->fd, EVFILT_EXCEPT,					\
			EV_##op, 0, 0, pe->data, 0, 0);							\
	}																\
	if (skp_unlikely(!nr)) return 0;								\
	rc = kevent64(reactor->kqfd, &ke[0], nr, NULL, 0, 0, NULL);		\
	if (skp_unlikely(rc < 0)) {										\
		log_warn("kevent64/%s failed : %s", #op, strerror_local());	\
		return -errno;												\
	}																\
	return 0;														\
}

__define_kqueue_ctl(register, ADD)
__define_kqueue_ctl(unregister, DELETE)
__define_kqueue_ctl(disable, DISABLE)
__define_kqueue_ctl(enable, ENABLE)

#undef __define_kqueue_ctl

/*错误返回非零0*/
static int reactor_create(struct uev_slot *slot)
{
	struct kqueue_reactor *
		reactor = malloc(sizeof(*reactor) +
			sizeof(reactor->ready_events[0]) * slot->ready_size);
	if (skp_unlikely(!reactor))
		return -ENOMEM;
	/*创建多路复用对象*/
	reactor->kqfd = kqueue();
	if (skp_unlikely(reactor->kqfd < 0)) {
		free(reactor);
		return -EINVAL;
	}
	reactor->ready_size = slot->ready_size;
	slot->reactor = reactor;

	return 0;
}

/*销毁不同实现的私有数据*/
static void reactor_destroy(struct uev_slot *slot)
{
	struct kqueue_reactor * reactor = slot->reactor;
	if (skp_likely(reactor)) {
		close(reactor->kqfd);
		free(reactor);
	}
}

static int reactor_modify_event(struct uev_slot *slot,
		struct uev_stream *ue/*old mask*/, struct poll_event *pe/*new mask*/)
{
	int rc, nr = 0;
	uint16_t delmask, addmask;
	struct kevent64_s ke[4];
	struct kqueue_reactor *reactor = slot->reactor;

	/*
	 *1. close same mask
	 *2. calculate finally mask
	 */
	delmask = (ue->mask ^ pe->mask) & ue->mask;
	addmask = (ue->mask ^ pe->mask) & pe->mask;

	if (delmask & EVENT_READ)
		EV_SET64(&ke[nr++],ue->fd,EVFILT_READ,EV_DELETE,0,0,pe->data,0,0);
	if (delmask & EVENT_WRITE)
		EV_SET64(&ke[nr++],ue->fd,EVFILT_WRITE,EV_DELETE,0,0,pe->data,0,0);

	if (addmask & EVENT_READ)
		EV_SET64(&ke[nr++],ue->fd,EVFILT_READ,EV_ADD,0,0,pe->data,0,0);
	if (addmask & EVENT_WRITE)
		EV_SET64(&ke[nr++],ue->fd,EVFILT_WRITE,EV_ADD,0,0,pe->data,0,0);

	if (nr == 0)
		return 0;

	rc = kevent64(reactor->kqfd, &ke[0], nr, NULL, 0, 0, NULL);

	if (skp_unlikely(rc < 0)) {
		log_warn("kevent64/modify failed : %s", strerror_local());
		return -errno;
	}

	return rc;
}

static int reactor_poll(struct uev_slot *slot, int timeout)
{
	int nr;
	struct kevent64_s *ke;
	struct poll_event *pe;
	struct uev_siginfo *siginfo;
	struct timespec ts, *pts = NULL;
	struct kqueue_reactor *reactor = slot->reactor;

	if (timeout > -1) {
		ts.tv_sec = timeout / 1000;
		ts.tv_nsec = (timeout % 1000) * 1000 * 1000;
		pts = &ts;
	}

	siginfo = slot->siginfo;
	if (siginfo)
		bitmap_zero(siginfo->active, NSIG);

	nr = kevent64(reactor->kqfd, NULL, 0,
			&reactor->ready_events[0], reactor->ready_size, 0, pts);

	if (skp_unlikely(nr < 0)) {
		if (skp_likely(errno == EINTR))
			return 0;
		log_warn("kevent64 failed : %s", strerror_local());
		return -errno;
	}

	EVENT_BUG_ON(nr > slot->ready_size);

	for (int i = 0; i < nr; i++) {
		pe = &slot->ready_events[i];
		ke = &reactor->ready_events[i];
		kqev2mask(ke, pe);
		if (skp_unlikely(pe->mask & EVENT_R0)) {
			/*收集触发的信号*/
			EVENT_BUG_ON(!siginfo);
			EVENT_BUG_ON(ke->ident >= NSIG);
			EVENT_BUG_ON(pe->mask & ~ EVENT_R0);
			EVENT_BUG_ON(poll_get_id(pe) != EV_SIGID);
			EVENT_BUG_ON(poll_get_fd(pe) != ke->ident);
			__set_bit(ke->ident, siginfo->active);
		}
	}

	return nr;
}

#define __define_kqueue_ctl(name, op)										\
static inline int __reactor_##name##_signal(struct uev_slot *slot, 			\
		int signo)															\
{																			\
	int rc;																	\
	struct kevent64_s ke;													\
	uint64_t data = poll_mk_data(EV_SIGID, signo);							\
	struct kqueue_reactor *reactor = slot->reactor;							\
	EVENT_BUG_ON(!slot->siginfo);											\
	EV_SET64(&ke, signo, EVFILT_SIGNAL, EV_##op, 0, 0, data, 0, 0);			\
	rc = kevent64(reactor->kqfd, &ke, 1, NULL, 0, 0, NULL);					\
	if (skp_unlikely(rc < 0)) {												\
		log_warn("kevent64/%s failed : %s", #op, strerror_local());			\
		return -errno;														\
	}																		\
	return 0;																\
}

__define_kqueue_ctl(register, ADD)
__define_kqueue_ctl(unregister, DELETE)

#undef __define_kqueue_ctl

static int reactor_register_signal(struct uev_slot *slot, int signo)
{
	struct kqueue_reactor *reactor = slot->reactor;
	int rc = __reactor_register_signal(slot, signo);
	/*交由 poller 处理信号，保存原来的信号处理句柄*/
	if (skp_likely(!rc))
		reactor->signal_defhdl[signo] = signal_setup(signo, SIG_IGN);
	return rc;
}

static int reactor_unregister_signal(struct uev_slot *slot, int signo)
{
	struct kqueue_reactor *reactor = slot->reactor;
	int rc = __reactor_unregister_signal(slot, signo);
	/*恢复原来的信号处理句柄*/
	if (skp_likely(!rc))
		signal_setup(signo, reactor->signal_defhdl[signo]);
	return rc;
}
