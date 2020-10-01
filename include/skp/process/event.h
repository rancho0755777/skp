/*
 * 所有事件都应该是不频繁的调用，所以尽量使用非内联函数来实现各种便捷函数
 */
#ifndef __US_EVENT_H__
#define __US_EVENT_H__

#include "../utils/uref.h"
#include "../utils/atomic.h"
#include "../adt/list.h"
#include "../adt/heap.h"

__BEGIN_DECLS

/*如果需要单线程模式的事件模型，需要自行调用 sysevent_init()*/
extern bool sysevent_up;
extern void __sysevent_init(bool single);
static inline void sysevent_init(bool single)
{
	if (skp_unlikely(!READ_ONCE(sysevent_up)))
		__sysevent_init(single);
}

/*事件对象*/
struct rcu_head;
struct uev_stream;
struct uev_timer;
struct uev_signal;
struct uev_async;
/*事件回调*/
typedef void (*uev_timer_fn)(struct uev_timer*);
typedef void (*uev_async_fn)(struct uev_async *);
typedef void (*uev_signal_fn)(struct uev_signal *);
typedef void (*rcu_callback_fn)(struct rcu_head *);
typedef void (*uev_stream_fn)(struct uev_stream*, uint16_t mask);

/*
 * socket/pipe 只使用 EVENT_READ/EVENT_WRITE
 * signal 只使用 EVENT_SIGNAL
 * file 使用 event_file 标记，并且使用 NOTE_READ/NOTE_WRITE... 来侦听文件的特殊事件
 */

enum {
	/*TODO:file*/

	/*event type*/
	EVENT_STREAM = 0,
	EVENT_TIMER,
	EVENT_SIGNAL,
	EVENT_ASYNC,
	EVENT_TYPE_MASK = 0x3U,

	/*event status*/
	EVENT_PENDING_BIT = ilog2(EVENT_TYPE_MASK + 1),
	EVENT_ATTACHED_BIT,
	EVENT_TIMEDOUT_BIT = EVENT_ATTACHED_BIT,
	EVENT_WRITE_ONCE_BIT, /**< 执行回调前 disable*/
	EVENT_IDX_SHIFT,

	/*event dispatched index*/
	EVENT_IDX_MAX = 0xffU,
	EVENT_IDX_MASK = EVENT_IDX_MAX << EVENT_IDX_SHIFT,
	EVENT_FLAG_BITS = EVENT_IDX_SHIFT + ilog2(EVENT_IDX_MAX + 1),

	/*event mask*/

	/*event active */
	EVENT_READ = 0x01U,
	EVENT_WRITE = 0x02U,
	/*return flags*/
	EVENT_ERROR = 0x04U,
	EVENT_EOF = 0x08U,
	/*内部使用 reserve*/
	EVENT_R0 = 0x10U,
	EVENT_R1 = 0x20U,
	EVENT_R3 = 0x40U,
	/*trigger behavior*/
	EVENT_EDGE = 0x80U,
	/*mask bits*/
	EVENT_MASK = (EVENT_EDGE - 1),
	EVENT_ACTION_MASK = EVENT_READ | EVENT_WRITE,
};

/*已加入 reactor 事件模型中*/
#define EVENT_PENDING	(1UL << EVENT_PENDING_BIT)
#define EVENT_WRITE_ONCE (1UL << EVENT_WRITE_ONCE_BIT)
/*如果是流事件 已加入 kernel 层次的多路复用*/
#define EVENT_ATTACHED	(1UL << EVENT_ATTACHED_BIT)
#define EVENT_TIMEOUTED	(1UL << EVENT_TIMEOUTED_BIT)
#define EVENT_FLAG_MASK ((1UL << EVENT_FLAG_BITS) - 1)

struct uev_core {
	unsigned long flags;
};

#define uev_flags_idx(flags)						\
	((uint32_t)((READ_ONCE((flags)) & EVENT_IDX_MASK) >> EVENT_IDX_SHIFT))

#define uev_core_idx(core) uev_flags_idx((core)->flags)
#define uev_core_pending(core) ({					\
	smp_rmb();										\
	!!test_bit(EVENT_PENDING_BIT, &(core)->flags);	\
})

/*
 * 设置期望事件上下文运行的CPU号，如果已经设置则忽略
 * 必须保证事件对象已经完全停止调度才能运行
 * 1. 关联的事件回调，调用此函数时，必须保证实现删除事件。
 * 2. 已设置后，如果需要改变，则重新初始化才能设置。
 */
extern int uev_core_setcpu(struct uev_core *event, int cpu);

////////////////////////////////////////////////////////////////////////////////
/* 流事件
 * 流事件关联了内核对象，可能出现poller收集了数据，但是事件被异步删除的情况，
 * 所以关联了一个短时间内不会被复用的ID来标识这样的情况。
 */
struct uev_stream {
	struct uev_core core;
	int32_t fd;
	uint16_t id;
	uint16_t mask;
	uev_stream_fn func;
};

#define __UEV_STREAM_INITIALIZER(_fd, _func) 			\
{														\
	.core = { .flags = EVENT_IDX_MASK|EVENT_STREAM|		\
						EVENT_WRITE_ONCE, }, 			\
	.mask = 0, .fd = _fd, .id = U16_MAX, .func = _func, \
}

#define DEFINE_UEV_STREAM(_event, _fd, _func)			\
	struct uev_stream _event = __UEV_STREAM_INITIALIZER(_fd, _func)

static inline void
uev_stream_init(struct uev_stream *stream, int32_t fd, uev_stream_fn func)
{
	memset(stream, 0, sizeof(*stream));
	stream->fd = fd;
	stream->func = func;
	stream->id = U16_MAX;
	stream->core.flags = EVENT_IDX_MASK|EVENT_STREAM|EVENT_WRITE_ONCE;
}

/**
 * 异步修改的流的侦听事件，如果还没有被事件循环监控，则会添加事件循环中
 * @param mask EVENT_READ/EVENT_WRITE/EVENT_EDGE 的或集合
 * @return < 0 失败
 */
extern int __uev_stream_modify(struct uev_stream *, uint16_t mask);
/**
 * 从事件循环中删除对流事件的监控
 * 同步删除时，会等待回调函数执行完毕，并可以指定新的索引，用于迁移其他的CPU线程上
 * @param sync 是否为同步删除
 * @param index 同步删除时，安全的修改CPU索引
 * @return 返回0表示从未激活状态删除，返回 < 0表示出错，否则从激活状态删除
 */
extern int __uev_stream_delete(struct uev_stream *, bool sync, int index);

#define uev_stream_fd(e) ((e)->fd)
#define uev_stream_mask(e) (READ_ONCE((e)->mask)&(EVENT_ACTION_MASK|EVENT_EDGE))
#define uev_stream_getcpu(e) (uev_core_idx(&(e)->core))
#define uev_stream_setcpu(e, c) uev_core_setcpu(&(e)->core, (c))
#define uev_stream_pending(e) uev_core_pending(&(e)->core)
#define uev_stream_delete(e, s) __uev_stream_delete((e), (s), EVENT_IDX_MAX)
#define uev_stream_delete_sync(e) uev_stream_delete((e), true)
#define uev_stream_delete_async(e) uev_stream_delete((e), false)

/*原子的关闭管理的描述符*/
static inline void uev_stream_closefd(struct uev_stream *s)
{
	int fd = xchg(&uev_stream_fd(s), -1);
	if (fd > -1) close(fd);
}

static inline int uev_stream_modify(struct uev_stream *stream, uint16_t mask)
{
	if (uev_stream_pending(stream) && READ_ONCE(stream->mask) == mask)
		return 0;
	return __uev_stream_modify(stream, mask);
}

static inline int uev_stream_add(struct uev_stream *stream, uint16_t mask)
{
	if (uev_stream_pending(stream))
		return -EBUSY;
	return __uev_stream_modify(stream, mask);
}

/**启动指定的流事件，如果已经启动，则返回0，出错返回负数错误*/
extern int uev_stream_enable(struct uev_stream *, uint16_t mask);
/**停止指定的流事件，如果已经启动，则返回0，出错返回负数错误*/
extern int uev_stream_disable(struct uev_stream *, uint16_t mask);

////////////////////////////////////////////////////////////////////////////////
/*毫秒精度定时器*/
struct uev_timer {
	struct uev_core core;
	uint32_t expires;
	uev_timer_fn func;
	struct heap_inode node;
};

#define __UEV_TIMER_INITIALIZER(_utimer, _func) 		\
{ 														\
	.core = { .flags = EVENT_IDX_MASK | EVENT_TIMER },	\
	.expires = 0, .func = _func, .node = 				\
			__HEAP_INODE_INITIALIZER((_utimer).node, 0),\
}

#define DEFINE_UEV_TIMER(_utimer, _func) 				\
	struct uev_timer _utimer = __UEV_TIMER_INITIALIZER(_utimer, _func)

/*毫秒粒度的未来毫秒时间戳，以纳秒表示*/
static inline uint64_t uev_timer_future(long offset)
{
	uint64_t f = similar_abstime(0, offset);
	return rounddown(f, 1000000);
}

static inline void uev_timer_init(struct uev_timer *timer, uev_timer_fn func)
{
	memset(timer, 0, sizeof(*timer));
	timer->func = func;
	heap_inode_init(&timer->node, 0);
	timer->core.flags = EVENT_IDX_MASK | EVENT_TIMER;
}

/**
 * @return 返回0表示从未排队状态修改，返回 < 0表示出错
 * 否则从排队状态修改，返回剩余的定时时间
 */
extern int __uev_timer_modify(struct uev_timer *utimer, uint32_t expires);
/**
 * 同步删除。且可以指定新的 索引，用于迁移
 * @return 返回0表示从未排队状态删除，返回 < 0表示出错
 * 否则从排队状态删除，返回剩余的定时时间
 */
extern int __uev_timer_delete(struct uev_timer *utimer, bool sync, int index);

#define uev_timer_getcpu(e) (uev_core_idx(&(e)->core))
#define uev_timer_pending(e) uev_core_pending(&(e)->core)
#define uev_timer_setcpu(e, c) uev_core_setcpu(&(e)->core, c)
#define uev_timer_delete(e, s) __uev_timer_delete((e), (s), EVENT_IDX_MAX)
#define uev_timer_delete_sync(e) uev_timer_delete((e), true)
#define uev_timer_delete_async(e) uev_timer_delete((e), false)
#define uev_timer_timedout(e) (!!test_bit(EVENT_TIMEDOUT_BIT, &(e)->core.flags))

static inline int uev_timer_add(struct uev_timer *timer, uint32_t expires)
{
	if (uev_timer_pending(timer))
		return -EBUSY;
	return __uev_timer_modify(timer, expires);
}

static inline int uev_timer_modify(struct uev_timer *timer, uint32_t expires)
{
	if (timer->expires == expires && uev_timer_pending(timer))
		return 0;
	return __uev_timer_modify(timer, expires);
}

/*剩余时间*/
extern int uev_timer_remain(const struct uev_timer *timer);
/*流失时间*/
extern int uev_timer_escapes(const struct uev_timer *timer);

////////////////////////////////////////////////////////////////////////////////
// 通过在每个CPU上经历一个固定的时钟周期来模拟 RCU
////////////////////////////////////////////////////////////////////////////////
#define RCU_GRANULARITY (HZ)
#define RCU_MS (RCU_GRANULARITY / 1000)
#define RCU_US (RCU_MS * 1000)
#define RCU_NS (RCU_US * 1000)

/*内存消耗比较大，尽量只用于特殊的句柄异步操作*/
struct rcu_head {
	struct uref refcnt;
	uint64_t future;
	rcu_callback_fn func;
	DEFINE_PER_CPU(struct list_head, node);
};

/**
 * 会企图锁住所有的事件线程，然后插入回调；
 * 除非实在需要一个延迟异步调用，否则尽量不要频繁调用
 */
extern void call_rcu_sched(struct rcu_head *, rcu_callback_fn);
/*rcu 异步释放*/
extern void rcu_free(void *ptr, void (*fn)(void*));
/*rcu 等待*/
extern void rcu_barrier(void);

////////////////////////////////////////////////////////////////////////////////
/*信号事件，仅 0 号CPU事件线程会调度信号*/
struct uev_signal {
	struct uev_core core;
	int32_t signo;
	uev_signal_fn func;
};

#define __UEV_SIGNAL_INITIALIZER(_signo, _func)			\
{ 														\
	.core = { .flags = EVENT_IDX_MASK | EVENT_SIGNAL },	\
	.signo = _signo, .func = _func,						\
}

#define DEFINE_UEV_SIGNAL(_usignal , _signo, _func) 	\
	struct uev_signal _usignal = __UEV_SIGNAL_INITIALIZER(_signo, _func)

extern int __uev_signal_register(struct uev_signal *);
extern int __uev_signal_unregister(struct uev_signal *, bool sync);

static inline void
uev_signal_init(struct uev_signal *signal, int signo, uev_signal_fn func)
{
	memset(signal, 0, sizeof(*signal));
	signal->func = func;
	signal->signo = signo;
	signal->core.flags = EVENT_IDX_MASK | EVENT_SIGNAL;
}

#define uev_signal_pending(s) uev_core_pending(&(s)->core)
#define uev_signal_unregister_sync(s) __uev_signal_unregister((s), true)
#define uev_signal_unregister_async(s) __uev_signal_unregister((s), false)

static inline int uev_signal_register(struct uev_signal *signal)
{
	if (uev_signal_pending(signal))
		return -EBUSY;
	return __uev_signal_register(signal);
}

/*信号等待挂起辅助函数，仅能在主线程中使用*/
extern int uev_sigwait_timeout(int signo, int timeout);
#define uev_sigwait(signo) uev_sigwait_timeout((signo), -1)

////////////////////////////////////////////////////////////////////////////////
/*异步通知事件，使用管道实现，不能等同于任务队列大量的使用，否则会消耗太多的资源*/
struct uev_async {
	struct uev_stream stream;
	atomic_t nr_cnt;
	int pipe_fd[2];
	uev_async_fn func;
};

#define __UEV_ASYNC_INITIALIZER(_func) 						\
{															\
	.stream = { .core = { .flags = EVENT_IDX_MASK|			\
		EVENT_ASYNC | EVENT_WRITE_ONCE , },					\
		.mask = 0, .fd = -1, .id = U16_MAX, 				\
		.func = async_stream_cb, 							\
	}, .nr_cnt = ATOMIC_INIT(-1), .pipe_fd = { -1, -1, }, 	\
	.func = _func,											\
}

#define DEFINE_UEV_ASYNC(_async, _func) \
	struct uev_async _async = __UEV_ASYNC_INITIALIZER(_func)

#define uev_async_getcpu(_nt) (uev_stream_getcpu(&(_nt)->stream))

extern void async_stream_cb(struct uev_stream *stream, uint16_t mask);
static inline void uev_async_init(struct uev_async *async, uev_async_fn func)
{
	memset(async, 0, sizeof(*async));
	async->func = func;
	atomic_set(&async->nr_cnt, -1);
	async->pipe_fd[0] = async->pipe_fd[1] = -1;
	uev_stream_init(&async->stream, -1, async_stream_cb);
	/*修正类型*/
	async->stream.core.flags = EVENT_IDX_MASK|EVENT_ASYNC|EVENT_WRITE_ONCE;
}

extern int __uev_async_register(struct uev_async *);
/*同步删除时会关闭 pipeline */
extern int __uev_async_unregister(struct uev_async *, bool sync);
/*触发通知*/
extern int __uev_async_emit(struct uev_async *async);

#define uev_async_pending(a) uev_stream_pending(&(a)->stream)
#define uev_async_setcpu(a, c) uev_stream_setcpu(&(a)->stream, (c))
#define uev_async_unregister_sync(a) __uev_async_unregister((a), true)
#define uev_async_unregister_async(a) __uev_async_unregister((a), false)

static inline int uev_async_register(struct uev_async *async)
{
	if (uev_async_pending(async))
		return -EBUSY;
	return __uev_async_register(async);
}

static inline int uev_async_emit(struct uev_async *async)
{
	if (!uev_async_pending(async))
		return -EINVAL;
	return __uev_async_emit(async);
}

////////////////////////////////////////////////////////////////////////////////
/*获取当前线程正在执行的相应类型的事件对象，如果不是相应类型的，则返回NULL*/
extern struct uev_stream* current_ev_stream(void);
extern struct uev_timer* current_ev_timer(void);
extern struct uev_signal* current_ev_signal(void);
extern struct uev_async* current_ev_async(void);
/*
 * 获取相应类型的目标事件所在的线程上正在执行的事件，如果没有事件正在被执行，
 * 则返回NULL
 * NOTE: 返回了一个临时的指针，因为其随时可能被销毁，所以仅用于标识，不可解引用
 */
extern const struct uev_stream* uev_ev_stream(struct uev_stream*);
extern const struct uev_timer* uev_ev_timer(struct uev_timer*);
extern const struct uev_signal* uev_ev_signal(struct uev_signal*);
extern const struct uev_async* uev_ev_async(struct uev_async*);

/*判断相应类型的指定事件是否在被执行*/
#define uev_stream_running(s) (uev_ev_stream((s)) == (s))
#define uev_timer_running(t) (uev_ev_timer((t)) == (t))
#define uev_signal_running(s) (uev_ev_signal((s)) == (s))
#define uev_async_running(a) (uev_ev_async((a)) == (a))

////////////////////////////////////////////////////////////////////////////////

__END_DECLS

#endif
