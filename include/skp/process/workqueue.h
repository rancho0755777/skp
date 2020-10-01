#ifndef __US_WORKQUEUE_H__
#define __US_WORKQUEUE_H__

#include "../utils/utils.h"
#include "../utils/atomic.h"
#include "event.h"
#include "thread.h"

__BEGIN_DECLS

/*工作队列内部是否创建 高优先级的 线程池*/
//#define WORKQUEUE_HAVE_HIGHPRI

/*
 * 移植 4.x 的 workqueue
 * 1. 基本结构
 *                  +-->[pool_queue]--+-->[worker_pool]-->[worker][worker][...]
 *  [workqueue]---->|-->[pool_queue]--|+->[worker_pool]-->[worker][worker][...]
 *                  +-->[pool_queue]--||+>[worker_pool]-->[worker][worker][...]
 *                                    |||
 *                  +-->[pool_queue]--+||
 *  [workqueue]---->|-->[pool_queue]---+|
 *                  +-->[pool_queue]----+
 * 2. 这个结构也是  work_struct 的流动方向
 */

enum {
	WQ_HIGHPRI = 1U << 0, /*使用高优先级的现在来处理工作，绑定线程的*/
	WQ_UNBOUND = 1U << 1, /*共享线程池，非绑定的工作队列*/

	/*以下为内部使用*/
	__WQ_ORDER = 1U << 2, /*顺序模式，一定是一个非绑定的工作队列*/
	__WQ_DRAINING = 1U << 3, /*正在被清理*/
	__WQ_DESTROYING = 1U << 4, /*正在被销毁*/

	WQ_MAX_ACTIVE = 64, /*每个WQ中最大排队数*/
	WQ_DFL_ACTIVE = WQ_MAX_ACTIVE / 2, /*默认WQ最大排队数，当创建参数中 active = 0 时使用*/
	WQ_MAX_UNBOUND_PER_CPU = 4, /*非绑定类型的WQ每个CPU最大排队数*/
	WQ_UNBOUND_PWQS = NR_CPUS / 2, /*非绑定类型的WQ中最大排队池数量*/
};

enum {
	WQ_WORK_LINKED_BIT = 0, /**<目标work正在被flush*/
	WQ_WORK_PWQ_BIT, /**<work正在等待被worker执行，flags 中存储的是所属的 pwq 对象指针，
					   *否则可能存储的是上一次运行的 pool id*/
	WQ_WORK_PENDING_BIT, /**<已经排队到workqueue*/
	WQ_WORK_DELAYED_BIT, /**<延迟标志，正在 pwq 延迟队列中*/
	WQ_WORK_CANCELING_BIT, /**<正在被取消*/
	WQ_WORK_BARRIER_BIT, /**<woke是一个内部使用的barrier*/
	WQ_WORK_FLAG_BITS,

	/* bit mask for work_busy() return values */
	WORK_BUSY_PENDING	= 1 << 0,
	WORK_BUSY_RUNNING	= 1 << 1,
	WQ_WORK_CPU_UNBOUND = -1, /*任意选择一个CPU执行该任务*/
};

#define WQ_WORK_LINKED		(1UL << WQ_WORK_LINKED_BIT)
#define WQ_WORK_PWQ			(1UL << WQ_WORK_PWQ_BIT)
#define WQ_WORK_PENDING		(1UL << WQ_WORK_PENDING_BIT)
#define WQ_WORK_DELAYED		(1UL << WQ_WORK_DELAYED_BIT)
#define WQ_WORK_CANCELING	(1UL << WQ_WORK_CANCELING_BIT)
#define WQ_WORK_BARRIER 	(1UL << WQ_WORK_BARRIER_BIT)
#define WQ_WORK_FLAG_MASK	((1UL << WQ_WORK_FLAG_BITS) - 1)
#define WQ_WORK_DATA_MASK	(~ WQ_WORK_FLAG_MASK) /*从来没有排队或执行过*/

struct work_struct;
struct workqueue_struct;
typedef void (*work_fn)(struct work_struct*);

#define WQ_STAT
//#define WQ_DEBUG

#ifdef WQ_STAT
enum {
	wq_work_start_sched,
	wq_work_finish_sched,
	wq_work_start_process,
	wq_work_finish_process,
	wq_work_start_cancel,
	wq_work_finish_cancel,
	wq_work_time_points,
};
#endif

struct work_struct {
	unsigned long flags;
	work_fn		func;
	struct list_head entry;
#ifdef WQ_STAT
	uint64_t	time_point[wq_work_time_points];
#endif
};


#ifdef WQ_STAT
#define __WORK_INITIALIZER(n, f)  {										\
		.flags = WQ_WORK_DATA_MASK, 									\
		.func = (f),													\
		.entry = LIST_HEAD_INIT((n).entry),								\
		.time_point = { },												\
	}
#define __INIT_WORK(_work, _func)										\
	do {																\
		(_work)->flags = WQ_WORK_DATA_MASK;								\
		(_work)->func = _func;											\
		INIT_LIST_HEAD(&(_work)->entry);								\
		memset((_work)->time_point, 0, sizeof((_work)->time_point));	\
	} while (0)
#else
#define __WORK_INITIALIZER(n, f)  {										\
		.flags = WQ_WORK_DATA_MASK,										\
		.func = (f),													\
		.entry = LIST_HEAD_INIT((n).entry),								\
	}
#define __INIT_WORK(_work, _func)										\
	do {																\
		(_work)->flags = WQ_WORK_DATA_MASK;								\
		(_work)->func = _func;											\
		INIT_LIST_HEAD(&(_work)->entry);								\
	} while (0)
#endif

#define DECLARE_WORK(n, f) \
	struct work_struct n = __WORK_INITIALIZER(n, f)

#define INIT_WORK(_work, _func) __INIT_WORK(_work, _func)

extern bool wq_online;
extern void __workqueue_init(bool single);

static inline void workqueue_init(bool single)
{
	if (skp_unlikely(!READ_ONCE(wq_online)))
		__workqueue_init(single);
}

/*
 * 不要直接使用这些变量，因为可能没有被初始化
 * 而是使用由以下宏定义的快捷函数
 * __defsched_work_func()
 * __defsched_delayedwork_func()
 */
extern struct workqueue_struct *system_wq;
extern struct workqueue_struct *system_highpri_wq;
extern struct workqueue_struct *system_long_wq;
extern struct workqueue_struct *system_unbound_wq;

/*不要直接使用此函数*/
extern struct workqueue_struct *
__alloc_workqueue(const char *fmt, uint32_t flags, int32_t max_active, ...)
__printf(1, 4);

/**
 * @param flags 标志 WQ_HIGHPRI, WQ_UNBOUND 的合集
 * @param max_active  workqueue 中的单个队列池中最大排队数，
 *        当为 1 且 flags & WQ_UNBOUND 为真是将创建一个顺序 workqueue
 */
#define alloc_workqueue(fmt, flags, max_active, ...)					\
	({																	\
		workqueue_init(false);											\
		__alloc_workqueue((fmt), (flags), (max_active), ##__VA_ARGS__);	\
	})
/**异步的销毁 workqueue，如果是对系统默认的 wq 调用将发出警告*/
extern void destroy_workqueue(struct workqueue_struct *);
/**等待当前调用时 wq 中排队的任务完成，不会查看后续排队的*/
extern void flush_workqueue(struct workqueue_struct *);
/**尽量等待当前排队的所有任务完成，但不保证函数调用后没有任务排队*/
extern void drain_workqueue(struct workqueue_struct *);
/**返回有多少个 任务队列 拥塞了*/
extern uint32_t workqueue_congested(struct workqueue_struct *);
////////////////////////////////////////////////////////////////////////////////

/**排队到指定 wq 的指定 cpu 上*/
extern bool queue_work_on(int cpu,struct workqueue_struct*,struct work_struct*);
/**同步取消 work，返回 false 表示没有排队，如果在排队会阻塞直到任务完成，并返回 true**/
extern bool __cancel_work_sync(struct work_struct *work, bool is_dwork);
/**异步取消 work，返回 false 表示没有排队，并返回 true**/
extern bool __cancel_work(struct work_struct *work, bool is_dwork);

/**冲洗任务，返回 false 表示没有排队，如果在排队会阻塞直到任务完成，并返回 true*/
extern bool flush_work(struct work_struct *work);
/**如果当前线程是工作线程，则会返回正在被执行的任务*/
extern struct work_struct *current_work(void);
////////////////////////////////////////////////////////////////////////////////

static inline bool work_pending(struct work_struct *work)
{
	return !!test_bit(WQ_WORK_PENDING_BIT, &work->flags);
}

/**
 * 查看当前任务是否在排队或正在被处理
 * @return OR'd bitmask of WORK_BUSY_* bits.
 */
extern uint32_t work_busy(struct work_struct *work);

static inline
bool queue_work(struct workqueue_struct *wq, struct work_struct *work)
{
	return queue_work_on(WQ_WORK_CPU_UNBOUND, wq, work);
}

static inline bool cancel_work_sync(struct work_struct *work)
{
	return __cancel_work_sync(work, false);
}

static inline bool cancel_work(struct work_struct *work)
{
	return __cancel_work(work, false);
}

////////////////////////////////////////////////////////////////////////////////
// 补充原语
////////////////////////////////////////////////////////////////////////////////

struct delayed_work {
	struct work_struct work;
	struct uev_timer timer;

	/* target workqueue and CPU ->timer uses to queue ->work */
	struct workqueue_struct *wq;
	int cpu;
};

extern void delayed_work_timer_cb(struct uev_timer*);

#define __INIT_DELAYED_WORK(_work, _func) 						\
	do {														\
		INIT_WORK(&(_work)->work, _func); 						\
		uev_timer_init(&(_work)->timer, delayed_work_timer_cb); \
	} while(0)

#define __DELAYED_WORK_INITIALIZER(_work, _func)				\
	{ 															\
		.work = __WORK_INITIALIZER((_work).work, (_func)), 		\
		.timer = __UEV_TIMER_INITIALIZER((_work).timer, 		\
			delayed_work_timer_cb), 							\
	}

#define DECLARE_DELAYED_WORK(_work, _func) 						\
	struct delayed_work _work = __DELAYED_WORK_INITIALIZER(_work, _func)

#define INIT_DELAYED_WORK(_work, _func) __INIT_DELAYED_WORK(_work, _func)

static inline struct delayed_work *to_delayed_work(struct work_struct *ptr)
{
	return container_of(ptr, struct delayed_work, work);
}

extern bool queue_delayed_work_on(int cpu, struct workqueue_struct*,
		struct delayed_work*, uint32_t delay);
extern bool mod_delayed_work_on(int cpu, struct workqueue_struct*,
		struct delayed_work*, uint32_t delay);

/**
 * 等待延迟工作完成，如果定时器未到期，则立马删除并排队工作，然后等待完成。
 */
extern bool flush_delayed_work(struct delayed_work *);

extern uint32_t delayed_work_busy(struct delayed_work *);

static inline bool cancel_delayed_work(struct delayed_work *dwork)
{
	return __cancel_work(&dwork->work, true);
}

static inline bool cancel_delayed_work_sync(struct delayed_work *dwork)
{
	return __cancel_work_sync(&dwork->work, true);
}

////////////////////////////////////////////////////////////////////////////////
struct rcu_work {
	struct work_struct work;
	struct rcu_head rcu;

	/* target workqueue ->rcu uses to queue ->work */
	struct workqueue_struct *wq;
};

#define INIT_RCU_WORK(_work, _func)	INIT_WORK(&(_work)->work, (_func))

extern bool queue_rcu_work(struct workqueue_struct *, struct rcu_work *);
extern bool flush_rcu_work(struct rcu_work*);
extern int schedule_on_each_cpu(work_fn func);

////////////////////////////////////////////////////////////////////////////////
// 排队到系统默认任务队列快捷函数
////////////////////////////////////////////////////////////////////////////////

#define __defsched_work_func(name)									\
static inline bool schedule_##name##work_on(int cpu, struct work_struct *work) \
{																	\
	workqueue_init(false);											\
	return queue_work_on(cpu, system_##name##wq, work); 			\
}																	\
static inline bool schedule_##name##work(struct work_struct *work)	\
{																	\
	return schedule_##name##work_on(WQ_WORK_CPU_UNBOUND, work);		\
}																	\
static inline void flush_scheduled_##name##work(void)				\
{																	\
	flush_workqueue(system_##name##wq);								\
}																	\
static inline void drain_scheduled_##name##work(void)				\
{																	\
	drain_workqueue(system_##name##wq);								\
}

__defsched_work_func()
__defsched_work_func(highpri_)
__defsched_work_func(long_)
__defsched_work_func(unbound_)

////////////////////////////////////////////////////////////////////////////////
#define __defsched_delayedwork_func(name)							\
static inline bool schedule_delayed_##name##work_on(				\
	int cpu, struct delayed_work *dwork, uint32_t delay)			\
{																	\
	workqueue_init(false);											\
	return queue_delayed_work_on(cpu, system_##name##wq, dwork, delay);			\
}																	\
static inline bool schedule_delayed_##name##work(					\
	struct delayed_work *dwork, uint32_t delay)						\
{																	\
	return schedule_delayed_##name##work_on(WQ_WORK_CPU_UNBOUND, dwork, delay);	\
}																	\
static inline bool modify_delayed_##name##work_on(					\
	int cpu, struct delayed_work *dwork, uint32_t delay)			\
{																	\
	workqueue_init(false);											\
	return mod_delayed_work_on(cpu, system_##name##wq, dwork, delay);			\
}																	\
static inline bool modify_delayed_##name##work(						\
	struct delayed_work *dwork, uint32_t delay)						\
{																	\
	return modify_delayed_##name##work_on(WQ_WORK_CPU_UNBOUND, dwork, delay);	\
}

__defsched_delayedwork_func()
__defsched_delayedwork_func(highpri_)
__defsched_delayedwork_func(long_)
__defsched_delayedwork_func(unbound_)

////////////////////////////////////////////////////////////////////////////////
struct work_stat {
	uint64_t dispatch_cost;
	uint64_t sched_cost;
	uint64_t process_cost;
	uint64_t cancel_cost;
};

#ifdef WQ_STAT
#define __defwork_stat_func(name) 									\
	static inline void name(struct work_struct *work)				\
	{ work->time_point[wq_##name] = get_cycles(); }
extern void work_acc_stat(struct work_stat *dst, const struct work_struct *);
#else
#define __defwork_stat_func(name)									\
	static inline void name(struct work_struct *work) { (void)0; }
static inline void work_acc_stat(
	struct work_stat *dst, const struct work_struct *work) { (void)0; }
#endif

__defwork_stat_func(work_start_sched)
__defwork_stat_func(work_finish_sched)
__defwork_stat_func(work_start_process)
__defwork_stat_func(work_finish_process)
__defwork_stat_func(work_start_cancel)
__defwork_stat_func(work_finish_cancel)

__END_DECLS

#endif
