#define _GNU_SOURCE
#include <fcntl.h>
#include <sys/time.h>
#include <sys/types.h>

#include <skp/utils/uref.h>
#include <skp/utils/spinlock.h>
#include <skp/utils/mutex.h>
#include <skp/utils/bitmap.h>
#include <skp/adt/idr.h>
#include <skp/process/event.h>
#include <skp/process/thread.h>
#include <skp/process/wait.h>
#include <skp/process/signal.h>
#include <skp/mm/slab.h>

#define uev_core_type(core) (READ_ONCE((core)->flags) & EVENT_TYPE_MASK)
#define uev_is_stream(core) (uev_core_type(core) == EVENT_STREAM)
#define uev_is_timer(core) (uev_core_type(core) == EVENT_TIMER)
#define uev_is_signal(core) (uev_core_type(core) == EVENT_SIGNAL)
#define uev_is_async(core) (uev_core_type(core) == EVENT_ASYNC)

#define __REACTOR_IMPL_MAGIC__ 0xdeadbeef
#define EVENT_DEBUG

#ifdef EVENT_DEBUG
# define EVENT_BUG_ON(x) BUG_ON(x)
# define EVENT_WARN_ON(x) WARN_ON(x)
# define check_remain_stream(slot) do { 							\
for (int i = 0; slot->nr_events && i < PER_CPU_EVENTS; i++) { 		\
	struct uev_event *event = idr_remove(&slot->evidr, i); 			\
	if (skp_unlikely(event)) 										\
		log_warn("stream %p was still left in event slot", event); 	\
}} while(0)
# define check_remain_signal(slot) do {								\
for (int i = 0; slot->siginfo && i < NSIG; i++) { 					\
	if (skp_unlikely(test_bit(i, slot->siginfo->registered))) 		\
		log_warn("signal %d was still in registering", i); 			\
}} while (0)
#else
# define EVENT_BUG_ON(x) ((void)(x))
# define EVENT_WARN_ON(x) ((void)(x))
# define check_remain_stream(slot)
# define check_remain_signal(slot)
#endif

#define EV_RUNNING 0
/*信号处理的槽位*/
#define EV_SIGSLOT 0
#define EV_SIGID U32_MAX

/*默认单个线程能管理事件最值*/
#define PER_POLL_EVENTS_MAX U16_MAX
/*事件线程默认检查时间，毫秒*/
#define EV_POLL_INTERVAL 5000u

/*单个线程最大能管理事件的数量*/
#ifndef CONFIG_PER_CPU_MAX_EVENTS
# define CONFIG_PER_CPU_MAX_EVENTS PER_POLL_EVENTS_MAX
#endif

/*每次从内核中抽取就绪事件的数量*/
#ifndef CONFIG_PER_POLL_EVENTS
# ifdef EVENT_DEBUG
#  define CONFIG_PER_POLL_EVENTS 32
# else
#  define CONFIG_PER_POLL_EVENTS 1024
# endif
#endif

/*每次从内核中抽取就绪事件的数量*/
#define PER_POLL_EVENTS CONFIG_PER_POLL_EVENTS
/*单个线程最大能管理事件的数量*/
#define PER_CPU_EVENTS CONFIG_PER_CPU_MAX_EVENTS

/*现线程模型中断遍历宏*/
#define BREAK_IF_SINGLE_MODE if (single_mode) break

struct poll_event {
	uint16_t mask;
	/*low 32bits is soft id, high 32bits is hard fd*/
	uint64_t data;
};

struct uev_worker {
	struct _thread thread;
	struct uev_core *current;
};

/*仅在0号CPU上处理信号*/
struct uev_siginfo {
	DECLARE_BITMAP(active, NSIG); /* 已激活待处理的，poller 私有的*/
	DECLARE_BITMAP(registered, NSIG);/* 已注册的*/
	struct uev_signal *uev_signals[NSIG]; /*用户层信息*/
};

struct uev_slot {
	mutex_t lock;
	uint32_t index;

	/*流事件*/
	uint32_t nr_events;
	uint32_t nr_triggers;
	uint32_t ready_size;
	struct idr evidr; /**< 流事件的id分配器*/

	/*定时器*/
	uint32_t nr_timer;
	uint32_t nr_timedout;
	struct heap timer_heap;
	struct list_head rcu_queue;

	/*内部通知事件*/
	struct uev_async notifier;

	uthread_t worker_thread;
	unsigned long flags;

	/*poller*/
	void *reactor; /**< reactor 实现句柄*/
	struct poll_event *ready_events; /*TODO:移除该数组，转而渐进的转换依赖具体OS的事件就绪数据*/
	struct uev_siginfo *siginfo; /*仅 0 号 CPU 上有此信号集*/

	wait_queue_head_t wait_queue; /**< 同步队列*/
} __cacheline_aligned;

#ifndef NSIG
# define NSIG 64
#endif

bool sysevent_up = false;
static bool single_mode = false;
static uint32_t dispatch_idx = 0;
static DEFINE_PER_CPU_AIGNED(struct uev_slot, uev_slots);

/*事件线程回调*/
static int worker_cb(void *arg);

#define slot_lock_init(s) mutex_init(&(s)->lock)
#define slot_lock(s) mutex_lock(&(s)->lock)
#define slot_trylock(s) mutex_trylock(&(s)->lock)
#define slot_unlock(s) mutex_unlock(&(s)->lock)
#define slot_maybe_contented(slot) (cond_resched_mutex(&(slot)->lock))

#define __def_core_flag_op(op)									\
static bool test_##op(struct uev_core *core)					\
{ return !!test_bit(EVENT_##op##_BIT, &core->flags); }			\
static void clear_##op(struct uev_core *core)					\
{ __clear_bit(EVENT_##op##_BIT, &core->flags); }				\
static bool test_clear_##op(struct uev_core *core)				\
{ return __test_and_clear_bit(EVENT_##op##_BIT, &core->flags); }\
static bool test_set_##op(struct uev_core *core)				\
{ return __test_and_set_bit(EVENT_##op##_BIT, &core->flags); }

__def_core_flag_op(PENDING)
__def_core_flag_op(ATTACHED)
__def_core_flag_op(TIMEDOUT)

#undef __def_core_flag_op

#define uev_mask_other(flags) \
	(READ_ONCE((flags)) & ~EVENT_IDX_MASK)

#define uev_core_mkidx(idx) \
	((((unsigned long)idx) << EVENT_IDX_SHIFT) & EVENT_IDX_MASK)

#define uev_core_setidx(core, idx) \
	WRITE_ONCE((core)->flags,uev_mask_other((core)->flags)|uev_core_mkidx(idx))

#define poll_get_id(e) ((uint32_t)((e)->data & ((1ULL << 32) - 1)))
#define poll_get_fd(e) ((uint32_t)((e)->data >> 32))
#define poll_mk_data(id, fd)					\
	(((uint64_t)(uint32_t)(id)) | ((uint64_t)(uint32_t)(fd)) << 32)
#define poll_event_init(e, i, f, m)				\
	do {										\
		(e)->mask = (m);						\
		(e)->data = poll_mk_data((i), (f));		\
	} while(0)

static inline struct uev_core *current_ev_core(void)
{
	struct uev_worker *worker = current_ev_worker();
	return worker ? worker->current : NULL;
}

static inline struct uev_core *get_current_core(struct uev_slot *slot)
{
	return READ_ONCE(((struct uev_worker*)(slot->worker_thread))->current);
}

static inline void set_current_core(struct uev_core *event)
{
	WRITE_ONCE(((struct uev_worker*)current)->current, event);
}

#define __def_get_currev(name, field)										\
static inline struct uev_##name *get_current_##name(struct uev_slot *slot)	\
{																			\
	struct uev_core *__core = get_current_core(slot);						\
	if (__core && uev_is_##name(__core))									\
		return container_of(__core, struct uev_##name, field);				\
	return NULL;															\
}

__def_get_currev(stream, core)
__def_get_currev(timer, core)
__def_get_currev(signal, core)
__def_get_currev(async, stream.core)

#undef __def_get_currev

static inline bool slot_running(struct uev_slot *slot)
{
	return test_bit(EV_RUNNING, &slot->flags);
}

static inline void create_worker(struct uev_slot *slot)
{
	struct uev_worker *worker;
	uthread_t thread = __uthread_create(worker_cb, slot, sizeof(*worker));
	BUG_ON(!thread);
	__set_bit(THREAD_ISEVENTWORKER_BIT, &thread->flags);
	static_mb();
	slot->worker_thread = thread;
	worker = (struct uev_worker*)thread;
	worker->current = NULL;
}

static inline struct uev_slot *loc_slot(int idx)
{
	struct uev_slot *slot = &per_cpu(uev_slots, idx);
	EVENT_BUG_ON(idx < 0);
	EVENT_BUG_ON(idx >= ARRAY_SIZE(uev_slots));
	return !slot_running(slot) ? NULL : slot;
}

static inline struct uev_slot *acquire_slot(struct uev_core *event)
{
	int32_t idx = uev_core_idx(event);
	/*自动设置*/
	sysevent_init(false);
	if (skp_unlikely(idx == EVENT_IDX_MAX)) {
		idx = uev_core_setcpu(event, idx);
		if (skp_unlikely(idx < 0))
			return NULL;
	}
	return loc_slot(idx);
}

#define put_slot_locked(s) slot_unlock((s))

static struct uev_slot *get_slot_and_lock(struct uev_core *core)
{
	struct uev_slot *slot;
	do {
		slot = acquire_slot(core);
		if (skp_unlikely(!slot))
			return NULL;
		slot_lock(slot);
		/*被同步修改了*/
		if (skp_likely(uev_core_idx(core) == slot->index))
			break;
		put_slot_locked(slot);
	} while (1);

	return slot;
}

static __always_inline
int stream_insert(struct uev_slot *slot, struct uev_stream *ev)
{
	long id;
	if (test_set_PENDING(&ev->core))
		return 0;
	/*new insert*/
	id = idr_alloc(&slot->evidr, ev);
	if (WARN_ON(id < 0)) {
		clear_PENDING(&ev->core);
		return -ENOMEM;
	}
	EVENT_BUG_ON(id > U16_MAX);
	ev->id = (uint16_t)id;
	slot->nr_events++;
	log_debug("insert event : fd [%d], id [%d]", ev->fd, ev->id);
	return 0;
}

static __always_inline
int stream_remove(struct uev_slot *slot, struct uev_stream *ev)
{
	struct uev_stream *tmp;
	if (!test_clear_PENDING(&ev->core))
		return 0;
	tmp = idr_remove(&slot->evidr, ev->id);
	EVENT_BUG_ON(tmp != ev);
	log_debug("remove event : fd [%d], id [%d]", ev->fd, ev->id);
	ev->id = U16_MAX;
	slot->nr_events--;
	return 1;
}

static __always_inline
struct uev_stream* stream_lookup(struct uev_slot *slot, int fd, uint32_t id)
{
	struct uev_stream *ev = idr_find(&slot->evidr, id);
	if (skp_unlikely(!ev || ev->fd != fd)) {
		log_warn_on(id != U32_MAX,
			"can't find event : fd [%d/%d], id [%d]", fd, (ev?ev->fd:-1), id);
		return NULL;
	}
	/*ABA cause this*/
	EVENT_BUG_ON(!uev_stream_pending(ev));
	EVENT_BUG_ON(uev_core_idx(&ev->core) != slot->index);
	return ev;
}

static __always_inline
bool timer_insert(struct uev_slot *slot, struct uev_timer *timer,
		uint32_t expires, uint64_t future)
{
	bool rc;
	EVENT_WARN_ON(!timer->func);
	slot->nr_timer++;
	timer->expires = expires;
	clear_TIMEDOUT(&timer->core);
	heap_inode_init(&timer->node, future);
	rc = iheap_insert(&timer->node, &slot->timer_heap);
	log_debug("insert timer %p/%u : expires : %u",
		timer, uev_core_idx(&timer->core), expires);
	return rc;
}

static __always_inline
int32_t timer_remove(struct uev_slot *slot, struct uev_timer *timer,
		uint32_t expires, uint64_t future)
{
	int32_t remain;
	uint64_t old, now;

	if (!test_clear_PENDING(&timer->core))
		return 0;

	slot->nr_timer--;
	old = timer->node.value;
	now = future - expires * 1000000;
	iheap_remove(&timer->node, &slot->timer_heap);
	remain = (int32_t)(((int64_t)old - now) / 1000000);

	log_debug("remove timer %p/%u : remain : %ld(us)",
		timer, uev_core_idx(&timer->core), (long)(old - now)/1000);

	return remain < 1 ? 1 : remain;
}

/*declare implement function*/
#define __declare_reactor_ctl(name) \
static int reactor_##name##_event(struct uev_slot*, \
	struct uev_stream*, struct poll_event*)

__declare_reactor_ctl(register);
__declare_reactor_ctl(unregister);
__declare_reactor_ctl(modify);
__declare_reactor_ctl(enable);
__declare_reactor_ctl(disable);

#undef __declare_reactor_ctl

/*错误返回非零0*/
static int reactor_create(struct uev_slot*);
/*销毁不同实现的私有数据*/
static void reactor_destroy(struct uev_slot*);
/*返回就绪个数*/
static int reactor_poll(struct uev_slot*, int timeout);

/*注册/注销信号事件*/
static int reactor_register_signal(struct uev_slot *, int signo);
static int reactor_unregister_signal(struct uev_slot *, int signo);

/*include implement file must be here*/
#if defined(__linux__)
# include "./reactor/epoll.h"
#elif defined(__apple__)
# include "./reactor/kqueue.h"
#else
# error "not support uevent"
#endif

static inline void insert_notifier(struct uev_slot *slot)
{
	struct poll_event pe;
	struct uev_async *async = &slot->notifier;
	struct uev_stream *stream = &async->stream;

	/*回调为空，仅仅为了中断 poller 使用*/
	uev_async_init(async, NULL);
	uev_core_setidx(&stream->core, slot->index);

	slot_lock(slot);
	BUG_ON(async->pipe_fd[0] != -1);
	BUG_ON(pipe2(async->pipe_fd, O_NONBLOCK|O_CLOEXEC));
	stream->mask = EVENT_READ;
	stream->fd = async->pipe_fd[0];
	BUG_ON(stream_insert(slot, stream));
	BUG_ON(test_set_ATTACHED(&stream->core));
	poll_event_init(&pe, stream->id, stream->fd, stream->mask);
	BUG_ON(reactor_register_event(slot, stream, &pe));
	slot_unlock(slot);
}

static inline void delete_notifier(struct uev_slot *slot)
{
	struct poll_event pe;
	struct uev_async *async = &slot->notifier;
	struct uev_stream *stream = &async->stream;
	slot_lock(slot);
	poll_event_init(&pe, stream->id, stream->fd, stream->mask);
	BUG_ON(!stream_remove(slot, stream));
	BUG_ON(!test_clear_ATTACHED(&stream->core));
	WARN_ON(reactor_unregister_event(slot, stream, &pe));
	close(async->pipe_fd[0]);
	close(async->pipe_fd[1]);
	slot_unlock(slot);
}

static inline void slot_invoke_start(struct uev_slot *slot,
		struct uev_core *core)
{
	set_current_core(core);
	slot_unlock(slot);
}

static inline void slot_invoke_finish(struct uev_slot *slot)
{
	slot_lock(slot);
	EVENT_BUG_ON(!get_current_core(slot));

	set_current_core(NULL);
	if (skp_likely(!waitqueue_active(&slot->wait_queue)))
		return;

	/*可能多个路径项删除定时器，并处于等待状态*/
	wake_up_all_locked(&slot->wait_queue);
	slot_unlock(slot);
	sched_yield();
	slot_lock(slot);
}

static __always_inline int32_t lookup_free_slot(void)
{
	int cpu;
	struct uev_slot *slot;
	for_each_possible_cpu(cpu) {
		slot = &per_cpu(uev_slots, cpu);
		if (skp_likely(idr_nr_free(&slot->evidr)))
			return cpu;
		BREAK_IF_SINGLE_MODE;
	}
	return -ENODEV;
}

static __always_inline
void stream_disable_locked(struct uev_slot *slot, struct uev_stream *stream,
		uint16_t mask)
{
	struct poll_event pe;

	if (!mask)
		return;

	mask &= (EVENT_ACTION_MASK | EVENT_EDGE);
	mask = stream->mask & (~mask);
	/*如果相等则忽略*/
	if (mask == stream->mask)
		return;

	poll_event_init(&pe, stream->id, stream->fd, mask);
	reactor_modify_event(slot, stream, &pe);
	WRITE_ONCE(stream->mask, mask);
}

static void process_streams(struct uev_slot *slot, int nr_ready)
{
	int j;
	uev_stream_fn func;
	unsigned long flags;
	uint16_t rmask, omask;
	struct uev_stream *ev;
	struct poll_event *pe;
	uint32_t nr_invokes = 0;

	if (skp_unlikely(!slot->nr_events) || !nr_ready)
		return;

	/*增加IO随机性*/
	j = prandom_int(0, nr_ready - 1);
	for (int i = 0; i < nr_ready; i++) {
		if (++j >= nr_ready)
			j = 0;

		pe = &slot->ready_events[j];
		/*内核返回的描述符与用户层的标识符形成唯一键值对，防止描述符被重用，而
		 *事件系统却不知道这个事实*/
		ev = stream_lookup(slot, poll_get_fd(pe), poll_get_id(pe));
		/*流事件可能被异步删除了*/
		if (skp_unlikely(!ev)) {
			slot_maybe_contented(slot);
			continue;
		}

		rmask = 0;
		nr_invokes++;

		omask = ev->mask;
		flags = ev->core.flags;
		if (skp_likely(flags & EVENT_ATTACHED)) {
#ifdef __apple__
			/*如果是边沿触发，从内核中删除本次已触发的侦听事件*/
			if (skp_unlikely(omask & EVENT_EDGE))
				rmask = pe->mask;
			else
#endif
			/*移除一次性写事件*/
			if (skp_likely(flags & EVENT_WRITE_ONCE) &&
				skp_likely(!(omask & EVENT_EDGE)) && (pe->mask & EVENT_WRITE))
				rmask = EVENT_WRITE;
			stream_disable_locked(slot, ev, rmask);
		}

		/*关注的事件可能被异步修改了*/
		if (skp_unlikely(!(pe->mask & omask)) &&
				!(pe->mask & (EVENT_ERROR | EVENT_EOF))) {
			slot_maybe_contented(slot);
			continue;
		}

		func = ev->func;
		slot_invoke_start(slot, &ev->core);
		if (skp_likely(func))
			func(ev, pe->mask);
		slot_invoke_finish(slot);
	}

	slot->nr_triggers += nr_invokes;

	return;
}

static inline uint32_t timer_timedout(struct uev_timer *timer, uint64_t now)
{
	if (!timer)
		return EV_POLL_INTERVAL;
	if (timer->node.value > now)
		return (uint32_t)((timer->node.value - now)/1000000);
	return 0;
}

#define first_timer(slot) \
	iheap_peek_entry(&(slot)->timer_heap, struct uev_timer, node);

/* TODO : 批量处理？
 * 返回下一次需要休眠的时间
 */
static void process_timers(struct uev_slot *slot, uint64_t now)
{
	uev_timer_fn func;
	struct uev_timer *timer = NULL;
	uint32_t remain = 0, nr_invokes = 0;

	if (!READ_ONCE(slot->nr_timer))
		return;

	do {
		timer = first_timer(slot);
		remain = timer_timedout(timer, now);
		if (remain)
			break;
		remain = timer_remove(slot, timer, 0, now);
		EVENT_BUG_ON(!remain);

		nr_invokes++;
		func = timer->func;
		test_set_TIMEDOUT(&timer->core);
		slot_invoke_start(slot, &timer->core);
		if (skp_likely(func))
			func(timer);
		slot_invoke_finish(slot);
	} while (1);

	slot->nr_timedout += nr_invokes;
}

static inline uint32_t rcu_timedout(struct rcu_head *rcu, uint64_t now)
{
	if (!rcu)
		return EV_POLL_INTERVAL;
	if (rcu->future > now)
		return (uint32_t)((rcu->future - now)/1000000);
	return 0;
}

#define first_rcu(queue, idx) \
	list_first_entry_or_null((queue), struct rcu_head, node[(idx)])

static void process_rcus(struct uev_slot *slot, uint64_t now)
{
	LIST__HEAD(queue);

	uint32_t remain = 0;
	rcu_callback_fn func;
	struct rcu_head *rcu;
	int cpu = slot->index;
	int nr_calls = current_ev_worker() ? (NR_CPUS * 64) : S32_MAX;

	if (list_empty(&slot->rcu_queue))
		return;

	/*由于rcu 不能（异步）取消，可以全部移动到栈空间，然后解锁处理，提高效率*/
	list_splice_init(&slot->rcu_queue, &queue);
	slot_unlock(slot);
	do {
		rcu = first_rcu(&queue, cpu);
		remain = rcu_timedout(rcu, now);
		if (remain)
			break;

		log_debug("rcu head timedout on %p : %d", rcu, slot->index);

		func = rcu->func;
		list_del_init(&rcu->node[cpu]);
		if (!single_mode && !__uref_put(&rcu->refcnt))
			continue;

		if (skp_likely(func))
			func(rcu);

	} while(--nr_calls > 0);

	if (nr_calls < 1)
		log_debug("too many rcu callback");

	slot_lock(slot);
	list_splice_init(&queue, &slot->rcu_queue);
}

static void process_signals(struct uev_slot *slot)
{
	unsigned long signo;
	uev_signal_fn func;
	struct uev_signal *signal;
	struct uev_siginfo *siginfo = slot->siginfo;

	/*遍历就绪的信号集合*/
	for_each_set_bit(signo, siginfo->active, NSIG) {
		/*可能已经异步的注销了信号事件*/
		if (!test_bit(signo, siginfo->registered))
			continue;

		signal = siginfo->uev_signals[signo];
		if (WARN_ON(!signal)) {
			__clear_bit(signo, siginfo->registered);
			continue;
		}

		/*锁内获取回调函数，函数可以异步修改*/
		func = signal->func;
		slot_invoke_start(slot, &signal->core);
		if (skp_likely(func))
			func(signal);
		slot_invoke_finish(slot);
	}
}

static inline void notify_poller(struct uev_slot *slot)
{
	int rc;
	/*仅不在同线程时才通知*/
	if (slot->worker_thread == current)
		return;
	rc = uev_async_emit(&slot->notifier);
	EVENT_BUG_ON(rc);
}

static inline void notify_poller_locked(struct uev_slot *slot, bool updated)
{
	/*TODO:判断是否在处理事件，以减少一次IO*/
	if (!get_current_core(slot))
		updated = false;
	put_slot_locked(slot);
	/*防止阻塞先解锁 */
	if (updated)
		notify_poller(slot);
}

static inline uint32_t calc_timeout(struct uev_slot *slot, uint64_t now)
{
	struct rcu_head *rcu;
	struct uev_timer *timer;
	int cpu = slot->index;
	uint32_t min_tt, min_rt;

	timer = first_timer(slot);
	rcu = first_rcu(&slot->rcu_queue, cpu);

	min_rt = rcu_timedout(rcu, now);
	min_tt = timer_timedout(timer, now);

	return min3(min_rt, min_tt, EV_POLL_INTERVAL);
}

static int worker_cb(void *arg)
{
	int nr_ready;
	uint64_t now;
	uint32_t next;
	sigset_t sigset;
	struct uev_slot *slot = arg;

	if (!single_mode)
		thread_bind(slot->index);

	BUG_ON(!current_ev_worker());
	BUG_ON(!slot || slot->worker_thread != current);

	next = EV_POLL_INTERVAL;
	signal_block_all(&sigset);

	while (!uthread_should_stop()) {
		nr_ready = reactor_poll(slot, next);
		if (WARN_ON(nr_ready < 0))
			break;

		now = uev_timer_future(0);
		/*process ready event*/
		slot_lock(slot);
		process_streams(slot, nr_ready);
		process_rcus(slot, now);
		process_timers(slot, now);
		/*仅0号CPU会处理信号*/
		if (slot->siginfo)
			process_signals(slot);
		next = calc_timeout(slot, now);
		slot_unlock(slot);
	}

	clear_bit(EV_RUNNING, &slot->flags);
	signal_unblock_all(&sigset);

	return 0;
}

static void __sysevent_finit(void)
{
	int cpu;
	struct uev_slot *slot;

	BUG_ON(current_ev_worker());

	if (WARN_ON(!sysevent_up))
		return;

	log_info("stopping event system");

	for_each_possible_cpu(cpu) {
		slot = &per_cpu(uev_slots, cpu);

		/*wakeup poller*/
		notify_poller(slot);

		/*stop thread*/
		uthread_stop(slot->worker_thread, NULL);

		/*invoke all of event*/
		slot_lock(slot);
		process_rcus(slot, 1);
		process_timers(slot, 1);
		slot_unlock(slot);

		delete_notifier(slot);

		/*检查流事件*/
		check_remain_stream(slot);
		/*检查信号*/
		check_remain_signal(slot);

		reactor_destroy(slot);
		free(slot->siginfo);
		free(slot->ready_events);

		log_info("trigger stream [%u] timer [%u] on cpu [%d]",
			slot->nr_triggers, slot->nr_timedout, cpu);
		BREAK_IF_SINGLE_MODE;
	}
}

void __sysevent_init(bool single)
{
	int cpu;
	struct uev_slot *slot;

	BUILD_BUG_ON(PER_CPU_EVENTS < 1);
	BUILD_BUG_ON(PER_CPU_EVENTS > PER_POLL_EVENTS_MAX);
	BUILD_BUG_ON(PER_POLL_EVENTS < 1);
	BUILD_BUG_ON(PER_POLL_EVENTS > PER_CPU_EVENTS);

	big_lock();
	if (skp_unlikely(sysevent_up)) {
		big_unlock();
		return;
	}

	single_mode = single;

	/*初始化基础设施*/
	for_each_possible_cpu(cpu) {
		log_info("initialize event system on cpu [%u]", cpu);

		slot = &per_cpu(uev_slots, cpu);
		slot->flags = 0;
		slot->index = cpu;
		slot->nr_events = 0;
		slot->nr_triggers = 0;
		slot->reactor = NULL;
		slot_lock_init(slot);
		init_waitqueue_head(&slot->wait_queue);

		/*timer*/
		slot->nr_timedout = 0;
		slot->nr_timer = 0;
		INIT_LIST_HEAD(&slot->rcu_queue);
		miniheap_init(&slot->timer_heap);

		/*signal*/
		if (cpu == EV_SIGSLOT) {
			slot->siginfo = malloc(sizeof(*slot->siginfo));
			BUG_ON(!slot->siginfo);
			memset(slot->siginfo, 0, sizeof(*slot->siginfo));
		}
		/*stream*/
		BUG_ON(idr_init_base(&slot->evidr, 0, PER_CPU_EVENTS - 1));
		slot->ready_size = min_t(int, PER_POLL_EVENTS, PER_CPU_EVENTS);
		slot->ready_events=malloc(sizeof(*slot->ready_events)*slot->ready_size);
		BUG_ON(!slot->ready_events);
		/*initialize reactor must be here*/
		BUG_ON(reactor_create(slot));

		/*install notifer*/
		insert_notifier(slot);

		create_worker(slot);
		BREAK_IF_SINGLE_MODE;
	}

	/*启动线程*/
	for_each_possible_cpu(cpu) {
		slot = &per_cpu(uev_slots, cpu);
		__set_bit(EV_RUNNING, &slot->flags);
		static_mb();
		BUG_ON(uthread_wakeup(slot->worker_thread));
		BREAK_IF_SINGLE_MODE;
	}

	WRITE_ONCE(sysevent_up, true);
	atexit(__sysevent_finit);
	big_unlock();
}

static inline void uev_core_chgidx(struct uev_core *core, int idx)
{
	unsigned long nflags, flags;
	if (idx < 0 && idx >= NR_CPUS)
		idx = EVENT_IDX_MAX;
	smp_rmb();
	flags = READ_ONCE(core->flags);
	if (WARN_ON(flags & EVENT_PENDING))
		return;
	nflags = uev_mask_other(flags) | uev_core_mkidx(idx);
	/*忽略错误*/
	WARN_ON(!try_cmpxchg(&core->flags, &flags, nflags));
}

static int uev_wait_finish_sync(struct uev_core *core, struct uev_slot *slot)
{
	int index;
	DEFINE_WAITQUEUE(wait);

	if (get_current_core(slot) != core)
		return 0;

	add_wait_queue_locked(&slot->wait_queue, &wait);

	do {
		if (get_current_core(slot) != core)
			break;
		slot_unlock(slot);
		log_debug("wait event finish : %p/%u", core, uev_core_idx(core));
		/*以超时等待，因为 事件模块 可能已经被停止*/
		wait_on_timeout(&wait, 2000);
		slot_lock(slot);
	} while (slot_running(slot));

	remove_wait_queue_locked(&slot->wait_queue, &wait);

	/*解锁了，有可能修改了 CPU 索引*/
	index = uev_core_idx(core);
	if (skp_unlikely(index != slot->index))
		return -EAGAIN;

	/* 回调又加入了事件系统
	 * 如果这时候 事件模块 处于正在停止状态，说明使用错误
	 */
	if (uev_core_pending(core))
		return -EBUSY;

	return 0;
}

/*查看事件池中所有的线程当前运行的事件*/
static void uev_slow_wait_finish(struct uev_core *core)
{
	int cpu, rc;
	struct uev_slot *slot;

	for_each_possible_cpu(cpu) {
		slot = &per_cpu(uev_slots, cpu);
		/*忽略当前遍历的 slot 是事件线程*/
		if (current == slot->worker_thread) {
			BREAK_IF_SINGLE_MODE;
			continue;
		}
		if (get_current_core(slot) != core) {
			BREAK_IF_SINGLE_MODE;
			continue;
		}
		slot_lock(slot);
		rc = uev_wait_finish_sync(core, slot);
		slot_unlock(slot);
		WARN_ON(rc == -EBUSY);
		BREAK_IF_SINGLE_MODE;
	}
}

int __uev_stream_modify(struct uev_stream *stream, uint16_t mask)
{
	int rc = 0;
	struct poll_event pe;
	struct uev_slot *slot;

	/*不允许使用不是输入的事件*/
	if (WARN_ON(mask & ~(EVENT_ACTION_MASK|EVENT_EDGE)))
		return -EINVAL;

	/*在锁外预分配*/
	radix_tree_preload();
	/*获取槽位，有可能因系统异步关闭而失败*/
	slot = get_slot_and_lock(&stream->core);
	if (skp_unlikely(!slot))
		return -ENODEV;

	/*插入到管理集合，分配了一个ID*/
	rc = stream_insert(slot, stream);
	if (skp_unlikely(rc))
		goto out;

	/*构造一个通用事件标识，用于与不同平台的内核通信*/
	poll_event_init(&pe, stream->id, stream->fd, mask);
	if (!test_set_ATTACHED(&stream->core)) {
		/*new register*/
		rc = reactor_register_event(slot, stream, &pe);
		if (skp_unlikely(rc))
			goto attach_fail;
	} else {
		/*modify*/
		if (skp_likely(mask || stream->mask) && mask != stream->mask) {
			rc = reactor_modify_event(slot, stream, &pe);
			if (skp_unlikely(rc))
				goto modify_fail;
		}

		pe.mask &= stream->mask;
		if ((stream->mask & EVENT_EDGE) &&
				skp_likely((pe.mask) & EVENT_ACTION_MASK)) {
			/*如果是边沿触发，调整侦听的新旧共有事件*/
			rc = reactor_enable_event(slot, stream, &pe);
			if (skp_unlikely(rc))
				goto modify_fail;
		}
	}

	WRITE_ONCE(stream->mask, mask);
	put_slot_locked(slot);
	return 0;

modify_fail:
	pe.mask = stream->mask;
	reactor_unregister_event(slot, stream, &pe);
attach_fail:
	clear_ATTACHED(&stream->core);
	stream_remove(slot, stream);
out:
	put_slot_locked(slot);
	return rc;
}

int __uev_stream_delete(struct uev_stream *stream, bool sync, int idx)
{
	int rc, rc2, index;
	struct uev_slot *slot;

	if (skp_unlikely(!READ_ONCE(sysevent_up)))
		return -ENODEV;

try:
	/*如果索引为无效值，那么可能处于不确定的状态，检查所有的事件线程*/
	index = uev_core_idx(&stream->core);
	if (skp_unlikely(index == EVENT_IDX_MAX))
		goto slow;

	slot = get_slot_and_lock(&stream->core);
	if (skp_unlikely(!slot))
		goto slow;

pending:
	/*unregister*/
	if (test_clear_ATTACHED(&stream->core)) {
		struct poll_event pe;
		poll_event_init(&pe, stream->id, stream->fd, stream->mask);
		WARN_ON(reactor_unregister_event(slot, stream, &pe));
	}

	rc = stream_remove(slot, stream);

	if (sync) {
		EVENT_BUG_ON(current_ev_stream() == stream);
		/*同步等待*/
		rc2 = uev_wait_finish_sync(&stream->core, slot);
		if (skp_unlikely(rc2)) {
			if (rc2 == -EAGAIN) {
				put_slot_locked(slot);
				goto try;
			}
			if (!slot_running(slot))
				goto out;
			goto pending;
		}
		uev_core_chgidx(&stream->core, idx);
	}

out:
	put_slot_locked(slot);
	return rc;

slow:
	/*将事件池全部检查一遍*/
	if (sync)
		uev_slow_wait_finish(&stream->core);
	return 0;
}

int uev_stream_enable(struct uev_stream *stream, uint16_t mask)
{
	uint16_t old;
	if (!uev_stream_pending(stream))
		return -EINVAL;
	mask &= (EVENT_ACTION_MASK | EVENT_EDGE);
	old = READ_ONCE(stream->mask);
	mask |= old;
	if ((mask == old
#ifdef __apple__
		 /*apple 需要手动的启动边沿触发*/
		 && !(mask & EVENT_EDGE)
#endif
		 ))
		return 0;
	return __uev_stream_modify(stream, mask);
}

int uev_stream_disable(struct uev_stream *stream, uint16_t mask)
{
	uint16_t old;
	if (!uev_stream_pending(stream))
		return -EINVAL;
	mask &= (EVENT_ACTION_MASK | EVENT_EDGE);
	old = READ_ONCE(stream->mask);
	mask = old & (~mask);
	if ((mask == old
#ifdef __apple__
		 /*apple 需要手动的启动边沿触发*/
		 && !(mask & EVENT_EDGE)
#endif
		 ))
		return 0;
	return __uev_stream_modify(stream, mask);
}

int __uev_timer_modify(struct uev_timer *timer, uint32_t expires)
{
	bool updated = true;
	uint32_t remain = 0;
	struct uev_slot *slot;
	uint64_t future = uev_timer_future(expires);

	slot = get_slot_and_lock(&timer->core);
	if (skp_unlikely(!slot))
		return -ENODEV;

try:
	if (!test_set_PENDING(&timer->core)) {
		updated = timer_insert(slot, timer, expires, future);
	} else {
		remain = timer_remove(slot, timer, expires, future);
		EVENT_BUG_ON(!remain);
		goto try;
	}
	/*wakeup poller*/
	notify_poller_locked(slot, updated);

	return remain > S32_MAX ? S32_MAX : remain;
}

int __uev_timer_delete(struct uev_timer *timer, bool sync, int idx)
{
	struct uev_slot *slot;
	int32_t remain, rc, index;
	uint64_t future = uev_timer_future(0);

	if (skp_unlikely(!READ_ONCE(sysevent_up)))
		return -ENODEV;

try:
	index = uev_core_idx(&timer->core);
	if (skp_unlikely(index == EVENT_IDX_MAX))
		goto slow;

	slot = get_slot_and_lock(&timer->core);
	if (skp_unlikely(!slot))
		goto slow;

pending:
	remain = timer_remove(slot, timer, 0, future);

	if (sync) {
		EVENT_BUG_ON(current_ev_timer() == timer);
		/*同步等待*/
		rc = uev_wait_finish_sync(&timer->core, slot);
		if (skp_unlikely(rc)) {
			if (rc == -EAGAIN) {
				put_slot_locked(slot);
				goto try;
			}
			if (!slot_running(slot))
				goto out;
			goto pending;
		}
		uev_core_chgidx(&timer->core, idx);
	}

out:
	put_slot_locked(slot);
	if (skp_unlikely(remain > S32_MAX))
		remain = S32_MAX;
	return remain;

slow:
	uev_slow_wait_finish(&timer->core);
	return 0;
}

/*剩余时间*/
int uev_timer_remain(const struct uev_timer *timer)
{
	int64_t now = uev_timer_future(0);
	if (timer->expires == U32_MAX)
		return INT32_MAX;
	return (int32_t)((int64_t) timer->node.value - now)/1000000;
}

/*流失时间*/
int uev_timer_escapes(const struct uev_timer *timer)
{
	return timer->expires != U32_MAX ?
		timer->expires - uev_timer_remain(timer) : 0;
}

int __uev_signal_register(struct uev_signal *signal)
{
	int rc = -EBUSY;
	struct uev_slot *slot;
	struct uev_siginfo *siginfo;
	uint32_t signo = signal->signo;

	if (WARN_ON(signo >= NSIG || signo < 1))
		return -EINVAL;

	EVENT_BUG_ON(signo == SIGSTOP);
	EVENT_BUG_ON(signo == SIGKILL);

	slot = get_slot_and_lock(&signal->core);
	if (skp_unlikely(!slot))
		return -ENODEV;

	siginfo = slot->siginfo;
	EVENT_BUG_ON(!siginfo);
	EVENT_BUG_ON(slot->index);
	if (!test_set_PENDING(&signal->core)) {
		/*已经注册过其他的处理句柄了*/
		if (!__test_and_set_bit(signo, siginfo->registered)) {
			rc = reactor_register_signal(slot, signo);
			if (skp_unlikely(rc))
				__clear_bit(signo, siginfo->registered);
		}
		if (skp_likely(!rc)) {
			siginfo->uev_signals[signo] = signal;
		} else {
			clear_PENDING(&signal->core);
		}
	}

	put_slot_locked(slot);

	return rc;
}

int __uev_signal_unregister(struct uev_signal *signal, bool sync)
{
	int rc = 0, rc2;
	uint32_t index, signo;
	struct uev_slot *slot;
	struct uev_siginfo *siginfo;

	if (skp_unlikely(!READ_ONCE(sysevent_up)))
		return -ENODEV;

try:
	index = uev_core_idx(&signal->core);
	if (skp_unlikely(index == EVENT_IDX_MAX))
		goto slow;

	slot = get_slot_and_lock(&signal->core);
	if (skp_unlikely(!slot))
		goto slow;

	signo = signal->signo;

	if (skp_unlikely(signo >= NSIG)) {
		rc = -EINVAL;
		goto out;
	}

	EVENT_BUG_ON(signo == SIGSTOP);
	EVENT_BUG_ON(signo == SIGKILL);

	siginfo = slot->siginfo;
	EVENT_BUG_ON(!siginfo);
	EVENT_BUG_ON(slot->index);

pending:
	if (test_clear_PENDING(&signal->core)) {
		rc = -ENOENT;
		if (__test_and_clear_bit(signo, siginfo->registered)) {
			EVENT_BUG_ON(siginfo->uev_signals[signo] != signal);
			siginfo->uev_signals[signo] = NULL;
			rc = reactor_unregister_signal(slot, signo);
			if (skp_likely(!rc))
				rc = 1;
		}
		if (skp_unlikely(rc < 1)) {
			log_warn("something was wrong ...");
			rc = 0;
		}
	}

	if (sync) {
		EVENT_BUG_ON(current_ev_signal() == signal);
		/*同步等待*/
		rc2 = uev_wait_finish_sync(&signal->core, slot);
		if (skp_unlikely(rc2)) {
			if (skp_unlikely(rc2 == -EAGAIN)) {
				put_slot_locked(slot);
				goto try;
			}
			if (!slot_running(slot))
				goto out;
			goto pending;
		}
		uev_core_chgidx(&signal->core, EVENT_IDX_MAX);
	}
out:
	put_slot_locked(slot);
	return rc;

slow:
	uev_slow_wait_finish(&signal->core);
	return 0;
}

struct sigwait {
	int signo;
	completion_t done;
	struct uev_signal base;
};

static void sigwait_cb(struct uev_signal *ptr)
{
	struct sigwait *sw = container_of(ptr, struct sigwait, base);
	log_info("interrupt by %d ...", sw->signo);
	complete(&sw->done);
}

int uev_sigwait_timeout(int signo, int timeout)
{
	int rc;
	struct sigwait sw;

	sw.signo = signo;
	init_completion(&sw.done);
	BUG_ON(!uthread_mainthread());
	uev_signal_init(&sw.base, signo, sigwait_cb);

	if ((rc = uev_signal_register(&sw.base)))
		return rc;

	rc = wait_for_completion_timeout(&sw.done, timeout);
	uev_signal_unregister_sync(&sw.base);

	return !rc ? -ETIMEDOUT : 0;
}

void async_stream_cb(struct uev_stream *stream, uint16_t mask)
{
	int nr;
	char buff[32];
	struct uev_async *async =
		container_of(stream, struct uev_async, stream);

	EVENT_BUG_ON(!uev_is_async(&stream->core));
	nr = atomic_xchg(&async->nr_cnt, -1);
	EVENT_WARN_ON(nr < 0);

	do {
		ssize_t b = read(async->pipe_fd[0], buff, ARRAY_SIZE(buff));
		if (skp_likely(b < ARRAY_SIZE(buff))) {
			EVENT_WARN_ON(!b);
			EVENT_WARN_ON(b < 0 && errno != EAGAIN);
			break;
		}
	} while ((nr=atomic_xchg(&async->nr_cnt, -1)));

	if (async->func)
		async->func(async);

#ifdef EVENT_DEBUG
	if (&per_cpu(uev_slots, uev_async_getcpu(async)).notifier == async)
		log_debug("eat notifier : %d", nr + 1);
#endif
}

int __uev_async_register(struct uev_async *async)
{
	if (uev_stream_pending(&async->stream))
		return 0;

	smp_rmb();
	sysevent_init(false);
	if (READ_ONCE(async->pipe_fd[0]) < 0) {
		int rc = 0;
		slot_lock(&per_cpu(uev_slots, 0));
		if (async->pipe_fd[0] < 0) {
			rc = pipe2(async->pipe_fd, O_NONBLOCK|O_CLOEXEC);
			if (skp_likely(!rc))
				async->stream.fd = async->pipe_fd[0];
		}
		slot_unlock(&per_cpu(uev_slots, 0));
		if (skp_unlikely(rc))
			return -errno;
	}

	return uev_stream_modify(&async->stream, EVENT_READ);
}

int __uev_async_unregister(struct uev_async *async, bool sync)
{
	/*是否可以关闭写端，触发读端 read == 0 来关闭*/
	int rc;

	if (skp_unlikely(!READ_ONCE(sysevent_up)))
		return -ENODEV;

	rc = uev_stream_delete(&async->stream, sync);
	/*从注册状态删除且为同步删除，则关闭 pipeline */
	if (rc && sync && READ_ONCE(async->pipe_fd[0]) > -1) {
		slot_lock(&per_cpu(uev_slots, 0));
		if (async->pipe_fd[0] > -1) {
			close(async->pipe_fd[0]);
			close(async->pipe_fd[1]);
			smp_wmb();
			WRITE_ONCE(async->pipe_fd[0], -1);
		}
		slot_unlock(&per_cpu(uev_slots, 0));
	}

	return rc;
}

/*触发通知*/
int __uev_async_emit(struct uev_async *async)
{
	if (skp_unlikely(!READ_ONCE(sysevent_up)))
		return -ENODEV;
	if (skp_unlikely(async->pipe_fd[1] < 0))
		return -EBADF;
	
	if (atomic_inc_and_test(&async->nr_cnt))
		if (write(async->pipe_fd[1], "1", 1) < 0) {
			if (errno != EAGAIN)
				return -errno;
		}

	return 0;
}

void call_rcu_sched(struct rcu_head *rcu, rcu_callback_fn func)
{
	int cpu;
	struct uev_slot *slot;
	const long toff = single_mode?RCU_MS*NR_CPUS:RCU_MS;

	BUG_ON(!func);

	sysevent_init(false);

	rcu->func = func;
	uref_set(&rcu->refcnt, NR_CPUS);
	rcu->future = uev_timer_future(toff);

	for_each_possible_cpu(cpu) {
		slot = loc_slot(cpu);
		if (skp_unlikely(!slot))
			goto call_out;
		slot_lock(slot);
		list_add_tail(&per_cpu(rcu->node, cpu), &slot->rcu_queue);
		notify_poller_locked(slot, true);
		BREAK_IF_SINGLE_MODE;
	}

	log_debug("rcu head sched : %p", rcu);
	return;

call_out:
	log_warn("RCU HEAD SCHED FAILED : %p", rcu);
	func(rcu);
	return;
}

struct entry {
	void *data;
	void (*free_fn)(void *ptr);
	struct rcu_head rcu;
};

static void rcu_free_cb(struct rcu_head *ptr)
{
	struct entry *entry = container_of(ptr, struct entry, rcu);
	entry->free_fn(entry->data);
	free(entry);
}

void rcu_free(void *ptr, void (*fn)(void*))
{
	struct entry *entry;

	BUG_ON(!fn);
	if (skp_unlikely(!ptr))
		return;

	entry = malloc(sizeof(*entry));
	BUG_ON(!entry);

	entry->data = ptr;
	entry->free_fn = fn;
	call_rcu_sched(&entry->rcu, rcu_free_cb);
}

struct barrier {
	struct rcu_head rcu;
	completion_t done;
};

static void rcu_barrier_cb(struct rcu_head * ptr)
{
	struct barrier *barr = container_of(ptr, struct barrier, rcu);
	log_debug("rcu barrier trigger");
	complete(&barr->done);
}

void rcu_barrier(void)
{
	struct barrier barr;
	init_completion(&barr.done);
	call_rcu_sched(&barr.rcu, rcu_barrier_cb);
	wait_for_completion(&barr.done);
}

int uev_core_setcpu(struct uev_core *core, int cpu)
{
	uint32_t hint = cpu; /*必须初始化*/
	unsigned long nflags, flags, idx;

	/*已经设置则忽略*/
	idx = uev_core_idx(core);
	if (idx >= 0 && idx < NR_CPUS)
		return idx;

	if (single_mode) {
		hint = 0;
	} else if (uev_is_signal(core)) {
		hint = EV_SIGSLOT;
	} else if (cpu < 0 || skp_unlikely(cpu >= NR_CPUS)) {
		cpu = thread_cpu();
		if (cpu > -1) {
			hint = cpu;
		} else {
			hint = xadd32(&dispatch_idx, 1);
		}
		hint &= (NR_CPUS-1);
	}

	EVENT_BUG_ON(hint < 0);
	EVENT_BUG_ON(hint >= ARRAY_SIZE(uev_slots));

	/*流事件需要检查ID是否足够*/
	if (uev_is_stream(core)) {
		struct uev_slot *slot = &per_cpu(uev_slots, hint);
		/*为了更好的防止ABA，保留一些槽位*/
		if (skp_unlikely(idr_nr_free(&slot->evidr) < 3)) {
			log_warn("slot [%d] not have enough id", hint);
			cpu = lookup_free_slot();
			if (skp_unlikely(cpu < 0))
				return -ENODEV;
			hint = cpu;
		}
	}

	/*原子的修改*/
	cpu = hint;
	idx = uev_core_mkidx(cpu);
	flags = READ_ONCE(core->flags);
	do {
		uint32_t oidx = uev_flags_idx(flags);
		if (skp_unlikely(oidx != EVENT_IDX_MAX))
			break;
		nflags = uev_mask_other(flags) | idx;
	} while (!try_cmpxchg(&core->flags, &flags, nflags));

	log_debug("uevent [%p] type [%s] has been dispatch to slot [%u]", core,
		uev_core_type(core) ? "timer" : "stream", cpu);

	return cpu;
}

#define __def_get_currev(name, field) 								\
struct uev_##name * current_ev_##name(void)							\
{																	\
	struct uev_core *__core = current_ev_core();					\
	if (__core && uev_is_##name(__core))							\
		return container_of(__core, struct uev_##name, field);		\
	return NULL;													\
}

__def_get_currev(stream, core)
__def_get_currev(timer, core)
__def_get_currev(signal, core)
__def_get_currev(async, stream.core)

#undef __def_get_currev

/*获取事件对象对应的事件线程正在执行的事件对象*/
#define __def_get_event(name, field) 								\
const struct uev_##name * uev_ev_##name(struct uev_##name *event)	\
{	struct uev_slot *slot; 											\
	uint32_t index = uev_core_idx(&event->field);					\
	if (index == EVENT_IDX_MAX)										\
		return NULL;												\
	slot = get_slot_and_lock(&event->field);						\
	if (skp_unlikely(!slot))										\
		return NULL;												\
	event = get_current_##name(slot);								\
	put_slot_locked(slot);											\
	return event;													\
}

__def_get_event(stream, core)
__def_get_event(timer, core)
__def_get_event(signal, core)
__def_get_event(async, stream.core)

#undef __def_get_event
