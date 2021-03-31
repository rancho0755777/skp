#ifndef __US_EVENT_H__
#define __US_EVENT_H__

#include "../adt/idr.h"
#include "../adt/list.h"
#include "../adt/heap.h"
#include "../utils/uref.h"
#include "../utils/mutex.h"
#include "../utils/bitmap.h"
#include "../utils/atomic.h"
#include "../process/wait.h"
#include "../process/thread.h"

__BEGIN_DECLS

/*事件对象*/
struct rcu_head;
struct uev_stream;
struct uev_timer;
struct uev_signal;
struct uev_async;
struct uev_looper;
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
	EVENT_FLAGS_SHIFT,
	EVENT_FLAGS_MASK = ((1U << EVENT_FLAGS_SHIFT) - 1),
	EVENT_EVLOOP_MASK = ~ EVENT_FLAGS_MASK,

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

	/*loop type*/
	EVLOOP_ONCE = 0x01U, /*只会运行一次*/
	EVLOOP_NOWAIT = 0x02U, /*不会因为陷入内核等待事件而阻塞*/
	/*looper flags*/
	EVLOOP_RUNNING = 0x01U,
	EVLOOP_PRIVATE = 0x02U, /*私有，操作不会加锁*/
	EVLOOP_SIGNAL = 0x04U, /*是否处理信号*/
};

/*已加入 reactor 事件模型中*/
#define EVENT_PENDING	(1UL << EVENT_PENDING_BIT)
#define EVENT_WRITE_ONCE (1UL << EVENT_WRITE_ONCE_BIT)
/*如果是流事件 已加入 kernel 层次的多路复用*/
#define EVENT_ATTACHED	(1UL << EVENT_ATTACHED_BIT)
#define EVENT_TIMEOUTED	(1UL << EVENT_TIMEOUTED_BIT)
#define EVENT_FLAG_MASK ((1UL << EVENT_FLAG_BITS) - 1)

/*事件对象*/
struct uev_core {
	unsigned long flags;
};

#define uev_flags_looper(flags)						\
	((struct uev_looper*)(READ_ONCE((flags)) & EVENT_EVLOOP_MASK))
#define uev_core_pending(core) 						\
	({smp_rmb();!!test_bit(EVENT_PENDING_BIT, &(core)->flags);})
#define uev_core_looper(core) uev_flags_looper((core)->flags)

/**
 * 设置期望事件上下文运行的事件循环，如果已经设置则忽略
 * 
 * 为了简单高效，并保证多线程安全，必须保证事件对象已经完全停止调度才能执行
 * 1. 关联的事件回调，调用此函数时，必须保证实现删除事件。
 * 2. 已设置后，如果需要改变，则重新初始化才能设置。
 * @param looper 如果 looper 没有空余的槽位，则会寻找一个合适的 looper
 *               为 NULL 则自动选择一个
 */
extern int uev_core_setlooper(struct uev_core *, struct uev_looper *);

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
	.core = { .flags = EVENT_STREAM|EVENT_WRITE_ONCE, },\
	.mask = 0, .fd = _fd, .id = U16_MAX, .func = _func, \
}

#define DEFINE_UEV_STREAM(_event, _fd, _func)			\
	struct uev_stream _event = __UEV_STREAM_INITIALIZER(_fd, _func)

extern void uev_stream_init(struct uev_stream *, int32_t fd, uev_stream_fn);

/**
 * 异步修改的流的侦听事件，如果还没有被事件循环监控，则会添加事件循环中
 * @param mask EVENT_READ/EVENT_WRITE/EVENT_EDGE 的或集合
 * @return < 0 失败
 */
extern int __uev_stream_modify(struct uev_stream *, uint16_t);

/**
 * 从事件循环中删除对流事件的监控
 * 同步删除时，会等待回调函数执行完毕，并可以指定新的索引，用于迁移其他的CPU线程上
 * @param sync 是否为同步删除
 * @param looper 同步删除时，安全的修改 looper
 * @return 返回0表示从未激活状态删除，返回 < 0表示出错，否则从激活状态删除
 */
extern int __uev_stream_delete(struct uev_stream *, bool, struct uev_looper*);

#define uev_stream_fd(e) ((e)->fd)
#define uev_stream_mask(e) (READ_ONCE((e)->mask)&(EVENT_ACTION_MASK|EVENT_EDGE))
#define uev_stream_looper(e) (uev_core_looper(&(e)->core))
#define uev_stream_setlooper(e, l) uev_core_setlooper(&(e)->core, (l))
#define uev_stream_pending(e) uev_core_pending(&(e)->core)
#define uev_stream_delete(e, s) __uev_stream_delete((e), (s), NULL)
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
/*异步通知事件，使用管道实现，不能等同于任务队列大量的使用，否则会消耗太多的资源*/
struct uev_async {
	struct uev_stream stream;
	atomic_t nr_cnt;
	int pipe_fd[2];
	uev_async_fn func;
};

#define __UEV_ASYNC_INITIALIZER(_func) 						\
{															\
	.stream = __UEV_STREAM_INITIALIZER(-1, async_stream_cb),\
	.nr_cnt = ATOMIC_INIT(-1), .pipe_fd = { -1, -1, }, 		\
	.func = _func,											\
}

#define DEFINE_UEV_ASYNC(_async, _func) \
	struct uev_async _async = __UEV_ASYNC_INITIALIZER(_func)

#define uev_async_looper(a) (uev_stream_looper(&(a)->stream))
#define uev_async_setlooper(a, l) (uev_stream_setlooper(&(a)->stream, l))
extern void async_stream_cb(struct uev_stream *stream, uint16_t mask);
extern void uev_async_init(struct uev_async *async, uev_async_fn func);

/*触发通知*/
extern int __uev_async_emit(struct uev_async *async);
static inline int uev_async_emit(struct uev_async *async)
{
	if (!uev_async_pending(async))
		return -EINVAL;
	return __uev_async_emit(async);
}

////////////////////////////////////////////////////////////////////////////////
/*loop相关*/

/*uev 线程*/
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

/*事件循环对象*/
struct uev_looper {
	mutex_t lock;
	uint32_t flags;
	
	uint32_t nr_events;
	uint32_t nr_triggers;
	uint32_t poll_size;
	/*流事件*/
	struct idr evidr; /**< 流事件的id分配器*/
	
	/*定时器*/
	uint32_t nr_timer;
	uint32_t nr_timedout;
	struct heap timer_heap;
	struct list_head rcu_queue;

	/*连接到全局的管理链表*/
	struct list_head node;
	/*内部通知事件*/
	struct uev_async notifier;

	/*poller*/
	void *reactor; /**< reactor 实现句柄*/
	struct uev_siginfo *siginfo; /*全系统 仅 0 号 CPU 上有此信号集*/

	/*status*/
	uthread_t worker_thread; /**< 已经绑定线程*/
	struct uev_core *current; /**< 当前处理事件*/

	wait_queue_head_t wait_queue; /**< 同步队列*/
} __aligned(1U << EVENT_FLAGS_SHIFT);

/**
 * 初始化 looper
 * @param looper 必须是 1U << EVENT_FLAGS_SHIFT 对齐的内存
 * @param poll_size 每次从内核中 拉取多少个就绪的事件，小于零则使用默认大小
 * @param flags 可以是无锁或不处理信号事件
 */
extern int uev_looper_init(struct uev_looper *, int poll_size, int flags);
extern void uev_looper_finit(struct uev_looper*);

/**
 * 动态分配创建
 */
extern struct uev_looper *uev_looper_create(int poll_size, int flags);
extern void uev_looper_destroy(struct uev_looper*);

/**
 * 绑定线程
 * 不能给私有的looper绑定线程?
 * @param thd 为空，则解绑
 */
extern void uev_looper_bind(struct uev_looper*, uthread_t thd);

/**
 * 循环
 * @param flags 是否等待或只循环一次
 */
extern int uev_loop(struct uev_looper*, int flags);


__END_DECLS

#endif
