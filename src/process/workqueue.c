#include <stdarg.h>
#include <skp/utils/uref.h>
#include <skp/utils/spinlock.h>
#include <skp/utils/rwlock.h>
#include <skp/utils/seqlock.h>
#include <skp/adt/idr.h>
#include <skp/adt/hlist_table.h>
#include <skp/process/signal.h>
#include <skp/process/completion.h>
#include <skp/process/workqueue.h>
#include <skp/mm/slab.h>

#ifdef WQ_DEBUG
# define WQ_WARN(x) WARN_ON((x))
# define WQ_BUG_ON(x) BUG_ON((x))
# define WQ_WARN_ON(x) WARN_ON((x))
#else
# define WQ_WARN(x) ((void)(x))
# define WQ_BUG_ON(x) ((void)(x))
# define WQ_WARN_ON(x) (false)
#endif

//#define WQ_POOL_USE_SPINLOCK

enum {

	POOL_DISASSOCIATED = 1U << 1, /**< 非静态全局 线程池 能被释放，且其中的所有线程都没有绑定CPU*/
	POOL_MANAGER_ACTIVE = 1U << 2,

	/*
	 * 状态机流程
	 * 1. 离开 prep，降低并发度
	 * 2. 进入 idle
	 * 3. 休眠
	 * 4. 被唤醒
	 * 5. 离开 idle
	 * 6. 查看是否有任务需要执行
	 * 7. 有任务则进入 prep 状态，增加并发度，否则跳转到第 2 步
	 * 8. 认领一个任务处理，知道没有任务可处理
	 * 9. 跳转到第 1 步，周而复始
	 */

	WORKER_DIE = 1U << 1, /*已被杀死，唤醒后将退出线程*/
	WORKER_IDLE = 1U << 2, /**< 空闲状态*/
	WORKER_PREP = 1U << 3, /**< 准备处理工作对象*/
	WORKER_UNBOUND = 1U << 4, /**< 非绑定的工作者线程*/

	WORKER_NOT_RUNNING = WORKER_PREP | WORKER_UNBOUND,

	MAX_IDLE_WORKERS_RATIO = 4,	/*1/4 of busy can be idle*/
	MAX_WORKERS_PER_POOL = NR_CPUS>4?NR_CPUS/2:NR_CPUS,
	IDLE_WORKER_TIMEOUT = 300 * HZ, /*300 s, keep idle ones for 5 mins*/

	BUSY_WORKER_HASH_ORDER = ilog2(MAX_WORKERS_PER_POOL),
	UNBOUND_POOL_HASH_ORDER = ilog2(NR_CPUS),

	NR_STD_WORKER_POOLS = 2, /**< 两类系统线程池，一个优先级高，另一个优先级低*/
	WQ_POOL_MIN_ID = 1U << WQ_WORK_FLAG_BITS,
	WQ_POOL_MAX_ID = U16_MAX - 1,
	WQ_POOL_INVALID_ID = WQ_POOL_MAX_ID + 1,

	CREATE_COOLDOWN = 2000 / HZ,
};

#define WQ_UNBOUND_MAX_ACTIVE \
	max_t(int, WQ_MAX_ACTIVE, get_cpu_cores() * WQ_MAX_UNBOUND_PER_CPU)

#define WQ_ALIGNED_SIZE (1U<<WQ_WORK_FLAG_BITS)

struct worker_pool;
struct pool_workqueue;
struct workqueue_struct;

struct wq_worker {
	struct _thread worker_thread;
	union {
		struct list_head entry; /*while idle*/
		struct hlist_node hentry; /*while busy*/
	};

	struct work_struct *curr_work; /**< 当前的工作对象*/
	work_fn				curr_func; /**< 当前的工作函数*/
	struct pool_workqueue *curr_pwq; /**< 当前工作对象的 队列池对象 curr_work's pwq*/

	struct list_head	scheduled; /*scheduled works*/

	struct worker_pool *pool; /**< associated pool*/
	struct list_head	node; /**< link to pool->workers*/
	struct timespec 	last_active; /**< last active timestamp*/
	uint32_t			flags; /*flags*/
	wait_queue_head_t	waitqueue; /**< idle wait on this waitqueue*/
};

struct worker_pool {
#ifdef WQ_POOL_USE_SPINLOCK
	spinlock_t		lock;
#else
	mutex_t			lock;
#endif
	int32_t			cpu;
	uint32_t		id;
	uint32_t		flags;
	uint32_t		nr_workers; /**< total number of worker thread*/
	uint32_t		nr_idles; /**< currently idle worker thread*/
	unsigned long	nr_triggers; /**< account of processing work*/

	struct list_head worklist; /**< list of pending works*/
	struct list_head idle_worker; /**< list of idle worker*/
	struct list_head workers; /**< all of attached workers*/
	struct hlist_node hash_node; /**< unbound_pool_hash node*/
	struct uev_timer idle_timer; /**< worker idle timeout*/

	mutex_t	attach_mutex; /**< attach/detach exclustion*/
	struct wq_worker *manager; /**< purely informational*/
	completion_t *detach_completion; /**< all worker detached*/

	PADDING(_pad0_)

	atomic_t nr_running;	/**< current concurrency level*/
	int32_t  refcnt;

	/*a woker is either on busy_worker or idle_worker, or the manager*/
	/**hash of busy workers*/
	DECLARE_HLIST_TABLE(busy_worker, BUSY_WORKER_HASH_ORDER);

	struct rcu_head		rcu;
} __aligned(WQ_ALIGNED_SIZE);

struct pool_workqueue {
	struct worker_pool *	pool;
	struct workqueue_struct * wq;

	int32_t  refcnt;
	uint32_t nr_active; /*nr of active works, not include scheduled work*/
	uint32_t max_active; /*max active works*/
	uint32_t nr_dispatch;

	struct list_head delayed_works; /**< 当已排队到worker上的工作超过 max_active时，将排队到 delayed works*/
	struct list_head pwqs_node; /*node on wq->pwqs*/

	/*release of unbound pwq is punted to system_wq*/
	struct work_struct release_work;
} __aligned(WQ_ALIGNED_SIZE);

struct workqueue_struct {
	uint32_t flags;
	uint32_t nr_drainers;
	mutex_t mutex;

	union {
		struct pool_workqueue *cpu_pwqs __percpu;	/**< private pwqs*/
		struct pool_workqueue *unbound_pwqs;	/**< private pwqs*/
	};

	int32_t		saved_max_active;
	/*用于 flush work queue*/
	int16_t		insert_seq;
	int16_t		remove_seq;
	wait_queue_head_t wait_flusher;

	char		name[32];
	struct list_head	pwqs; /*all pwqs of this wq, include shared pwqs*/
	struct list_head	list; /*list to workqueues*/
	struct rcu_head		rcu;
} __cacheline_aligned;

struct wfl_wait {
	wait_queue_t wait;
	int16_t	insert_seq; /*save snapshot of insert sequence*/
	int16_t	remove_seq; /*save snapshot of insert sequence*/
};

struct cwt_wait {
	wait_queue_t	wait;
	struct work_struct	*work;
};

struct wq_barrier {
	struct work_struct work;
	struct work_struct *whose;
	completion_t done;
};

/*系统共享工作线程池，其中的线程被绑定*/
bool wq_online = false;

static LIST__HEAD(workqueues); /*list of all workqueues*/

static struct idr wq_pool_idr;
#define WQ_POOL_IDR_USE_SEQLOCK
#ifdef WQ_POOL_IDR_USE_SEQLOCK
static DEFINE_SEQLOCK(wq_pool_idr_lock);
# define pool_idr_writelock() write_seqlock(&wq_pool_idr_lock)
# define pool_idr_writeunlock() write_sequnlock(&wq_pool_idr_lock)
# define pool_idr_readlock() read_seqbegin(&wq_pool_idr_lock)
# define pool_idr_readunlock(v) read_seqretry(&wq_pool_idr_lock, (v))
#else
static DEFINE_RWLOCK(wq_pool_idr_lock);
# define pool_idr_writelock() write_lock(&wq_pool_idr_lock)
# define pool_idr_writeunlock() write_unlock(&wq_pool_idr_lock)
# define pool_idr_readlock() (read_lock(&wq_pool_idr_lock), 1)
# define pool_idr_readunlock(v) (read_unlock(&wq_pool_idr_lock), false)
#endif
static DEFINE_MUTEX(wq_pool_mutex); /*protects pools and workqueues list*/
static DEFINE_WAIT_QUEUE_HEAD(wq_manager_wait); /*wait for manager to go away*/


#ifdef WQ_POOL_USE_SPINLOCK
# define pool_init_lock(p) (spin_lock_init(&(p)->lock))
# define pool_is_locked(p) (spinlock_is_locked(&(p)->lock))
# define lock_pool(p) (spin_lock(&(p)->lock))
# define unlock_pool(p) (spin_unlock(&(p)->lock))
#else
# define pool_init_lock(p) (mutex_init(&(p)->lock))
# define pool_is_locked(p) (mutex_is_locked(&(p)->lock))
/*必须是原子模式的锁，否则将产生死锁*/
static inline void lock_pool(struct worker_pool *p)
{ enter_atomic(); mutex_lock(&(p)->lock); }
static inline void unlock_pool(struct worker_pool *p)
{ mutex_unlock(&(p)->lock); leave_atomic(); }
#endif


/* the per-cpu worker pools, shared by all system
 * 一个二维的数组
 * 步长为 sizeof(struct wq_worker_pool[2])
 * 即对 cpu_wq_worker_pools[] 进行下标运算，一步将跨越 sizeof(struct wq_worker_pool[2])
 * 的大小
 */
static DEFINE_PER_CPU_AIGNED(struct worker_pool[NR_STD_WORKER_POOLS], cpu_wq_worker_pools);
/*hash of all unbound pools keyed by ptr of workqueue*/
static __cacheline_aligned DEFINE_HLIST_TABLE(unbound_pool_hash, UNBOUND_POOL_HASH_ORDER);

struct workqueue_struct *system_wq = NULL;
struct workqueue_struct *system_highpri_wq = NULL;
struct workqueue_struct *system_long_wq = NULL;
struct workqueue_struct *system_unbound_wq = NULL;

#ifdef WORKQUEUE_HAVE_HIGHPRI
# define __FECWP_SIZE__ NR_STD_WORKER_POOLS
#else
# define __FECWP_SIZE__ (1)
#endif

#define for_each_cpu_worker_pool(pool, cpu)								\
	for ((pool) = &per_cpu(cpu_wq_worker_pools,(cpu))[0];				\
		(pool) < &per_cpu(cpu_wq_worker_pools, (cpu))[__FECWP_SIZE__];	\
		(pool)++)

#define for_each_pwq(pwq, wq)											\
	list_for_each_entry(pwq, &(wq)->pwqs, pwqs_node)					\
		if (WQ_WARN_ON(!mutex_is_locked(&(wq)->mutex))) {} else

#define for_each_pwq_safe(pwq, next, wq)								\
	list_for_each_entry_safe(pwq, next, &(wq)->pwqs, pwqs_node)			\
		if (WQ_WARN_ON(!mutex_is_locked(&(wq)->mutex))) {} else

#define get_first_pwq(wq)												\
	list_first_entry_or_null(&(wq)->pwqs, struct pool_workqueue, pwqs_node)

#define rcu2wq(ptr) container_of(ptr,struct workqueue_struct,rcu)
#define rcu2wp(ptr) container_of(ptr, struct worker_pool, rcu)
#define timer2wp(t)	container_of((t), struct worker_pool, idle_timer)
#define relwork2pwq(w) container_of((w), struct pool_workqueue, release_work)
#define workerpool_lastidle(p) list_last_entry(&(p)->idle_worker, struct wq_worker, entry)
#define first_scheduledwork(w) list_first_entry(&(w)->scheduled, struct work_struct, entry)
#define workerpool_firstwork(p) list_first_entry(&(p)->worklist, struct work_struct, entry)
#define first_delayedwork(p) list_first_entry(&(p)->delayed_works, struct work_struct, entry)

/*在 work->flags 中编码数据和状态*/
static inline void set_work_data(struct work_struct *work, unsigned long data,
		unsigned long flags)
{
	//smp_wmb();
	WQ_WARN_ON(!work_pending(work));
	WRITE_ONCE(work->flags, data | flags);
	//smp_mb();
}

#define set_work_pwq(work, pwq, flags)				\
	set_work_data((work), (uintptr_t)(pwq),(flags)|WQ_WORK_PENDING|WQ_WORK_PWQ)

#define set_work_pool_and_clear_pending(work, pid)	\
	set_work_data((work), (pid) << WQ_WORK_FLAG_BITS, 0)

#define set_work_pool_and_keep_pending(work, pid)	\
	set_work_data((work), (pid) << WQ_WORK_FLAG_BITS, WQ_WORK_PENDING)

static inline uint32_t get_work_pool_id(struct work_struct *work)
{
	unsigned long flags = READ_ONCE(work->flags);
	if (flags & WQ_WORK_PWQ)
		return ((struct pool_workqueue*)(flags & WQ_WORK_DATA_MASK))->pool->id;
	return (uint32_t)((flags & WQ_WORK_DATA_MASK) >> WQ_WORK_FLAG_BITS);
}

static inline void mark_work_canceling(struct work_struct *work)
{
	uint32_t pid = get_work_pool_id(work);
	set_work_data(work, pid << WQ_WORK_FLAG_BITS,
		WQ_WORK_CANCELING | WQ_WORK_PENDING);
}

static inline bool work_is_canceling(struct work_struct *work)
{
	unsigned long flags = READ_ONCE(work->flags);
	return !(flags & WQ_WORK_PWQ) && (flags & WQ_WORK_CANCELING);
}

#define clear_work_data(work) set_work_data(work, WQ_WORK_DATA_MASK, 0);
#define pool_belong_syswq(p) (!(READ_ONCE((p)->flags) & POOL_DISASSOCIATED))

static inline bool wq_belong_syswq(struct workqueue_struct *wq)
{
	if (wq == system_wq || wq == system_highpri_wq ||
			wq == system_long_wq || wq == system_unbound_wq)
		return true;
	return false;
}

static inline bool pwq_belong_syswq(struct pool_workqueue *pwq)
{
	WQ_WARN(!wq_online || !pwq || !pwq->wq);
	return wq_belong_syswq(pwq->wq);
}

static inline bool too_many_workers(struct worker_pool *pool)
{
	bool managing = pool->flags & POOL_MANAGER_ACTIVE;
	/*管理线程算空闲线程？*/
	uint32_t nr_idles = pool->nr_idles + managing;
	uint32_t nr_busy = pool->nr_workers - nr_idles;
	return nr_idles > 2 && (nr_idles - 2) * MAX_IDLE_WORKERS_RATIO >= nr_busy;
}

/*所有的工作线程都休眠了（可能因为work的回调函数引起的） */
#define __need_more_worker(p) (!atomic_read(&(p)->nr_running))
#define need_more_worker(p) (!list_empty(&(p)->worklist)&&__need_more_worker(p))
#define may_start_working(p) ((p)->nr_idles||(p)->nr_workers>=MAX_WORKERS_PER_POOL)
#define need_to_create_worker(p) (need_more_worker((p)) && !may_start_working((p)))
/*仅保持一个线程处理任务，减少竞争*/
#define keep_working(p) (!list_empty(&(p)->worklist)&&atomic_read(&(p)->nr_running)<=1)
#define first_idle_worker(p) list_first_entry_or_null(&(p)->idle_worker, struct wq_worker, entry)

static void idle_worker_timeout(struct uev_timer*);
static void pwq_release_workfn(struct work_struct*);
static void put_unbound_pool(struct worker_pool *);
static struct wq_worker *create_worker(struct worker_pool *);

static inline void wake_up_worker(struct worker_pool *pool)
{
	struct wq_worker *worker = first_idle_worker(pool);
	if (skp_likely(worker))
		wake_up_one_locked(&worker->waitqueue);
}

static inline void worker_clr_flags(struct wq_worker *worker, uint32_t flags)
{
	struct worker_pool *pool = worker->pool;
	uint32_t oflags = worker->flags;

	worker->flags &= ~(flags);
	/* 递增并发度计数器
	 * 已确定 立马要处理认领的 任务
	 * 过去处于准备状态 和 本次想要清除准备状态
	 */
	if ((flags & WORKER_NOT_RUNNING) && (oflags & WORKER_NOT_RUNNING))
		if (!(worker->flags & WORKER_NOT_RUNNING)) {
			log_debug("inc concurrency of worker pool %p,"
				" worker %p turn to running", pool, worker);
			/*对于 unbound 肯定不会增加或减少 nr_running*/
			atomic_inc(&pool->nr_running);
		}
}

static inline void worker_set_flags(struct wq_worker *worker, uint32_t flags)
{
	struct worker_pool *pool = worker->pool;
	uint32_t oflags = worker->flags;

	/*递减并发度计数器*/
	if ((flags & WORKER_NOT_RUNNING) && !(oflags & WORKER_NOT_RUNNING)) {
		/*过去不处于准备状态 和 本此想要设置准备状态*/
		log_debug("dec concurrency of worker pool %p,"
			" worker %p turn to idle", pool, worker);
		atomic_dec(&pool->nr_running);
	}

	worker->flags |= (flags);
}

static inline struct pool_workqueue *get_pwq(struct pool_workqueue* pwq)
{
	WQ_BUG_ON(pwq && !pool_is_locked(pwq->pool));
	if (pwq && skp_unlikely(pwq->refcnt++ < 1)) {
		pwq->refcnt--;
		log_warn("workqueue has been destroyed : %p", pwq->wq);
		return NULL;
	}
	return pwq;
}

static inline void put_pwq(struct pool_workqueue* pwq)
{
	WQ_BUG_ON(pwq && !pool_is_locked(pwq->pool));
	if (skp_unlikely(!pwq) || skp_likely(--pwq->refcnt))
		return;
	/*不能释放系统创建的绑定类型的队列池，否则可能死锁*/
	if (WQ_WARN_ON(pwq_belong_syswq(pwq)))
		return;
	/*异步释放*/
	schedule_work(&pwq->release_work);
}

static inline void put_pwq_unlocked(struct pool_workqueue* pwq)
{
	if (skp_likely(pwq)) {
		struct worker_pool *pool = pwq->pool;
		lock_pool(pool);
		put_pwq(pwq);
		/*如果 pool 被联动释放 会等待 pool 解锁，所以 pool 一定不会失效*/
		unlock_pool(pool);
	}
}

/*当前运行在任务上下文，且工作队列与任务上下文所服务的工作队列相同*/
static inline bool is_chained_work(struct workqueue_struct *wq)
{
	struct wq_worker *worker = current_wq_worker();
	return worker && worker->curr_pwq->wq == wq;
}

static inline struct pool_workqueue *get_work_pwq(struct work_struct* work)
{
	unsigned long flags = READ_ONCE(work->flags);
	if (flags & WQ_WORK_PWQ)
		return (void*)(flags & WQ_WORK_DATA_MASK);
	return NULL;
}

static inline struct worker_pool *get_worker_pool(struct worker_pool* pool)
{
	/*pool 有效性判断*/
	if (!pool_belong_syswq(pool)) {
		mutex_lock(&wq_pool_mutex);
		if (WARN_ON(pool->refcnt++ < 1))
			pool = NULL;
		mutex_unlock(&wq_pool_mutex);
	}
	return pool;
}

static inline struct worker_pool *get_worker_pool_and_lock(struct worker_pool* pool)
{
	pool = get_worker_pool(pool);
	if (skp_likely(pool))
		lock_pool(pool);
	return pool;
}

/*此函数是一个优化竞争的典范*/
static struct worker_pool *get_work_pool(struct work_struct *work)
{
	uint32_t seq;
	struct worker_pool *pool = NULL;
	unsigned long flags = READ_ONCE(work->flags);
	unsigned long data_flags = flags & WQ_WORK_DATA_MASK;

	/*可能被取消了，所以 pwd 指针还被保留的*/
	if (flags & WQ_WORK_PWQ)
		return get_worker_pool(((struct pool_workqueue*)data_flags)->pool);

	/*pool's id*/
	data_flags = data_flags >> WQ_WORK_FLAG_BITS;
	do {
		if (skp_unlikely(data_flags < WQ_POOL_MIN_ID))
			return NULL;
		if (skp_unlikely(data_flags > WQ_POOL_MAX_ID))
			return NULL;
		seq = pool_idr_readlock();
		/*在锁内获取引用计数?*/
		pool = idr_find(&wq_pool_idr, data_flags);
	} while (pool_idr_readunlock(seq));

	smp_rmb();
	if (skp_likely(pool && READ_ONCE(pool->id) == data_flags))
		return get_worker_pool(pool);
	return NULL;
}

static inline struct worker_pool *get_work_pool_and_lock(struct work_struct *work)
{
	struct worker_pool *pool = get_work_pool(work);
	if (skp_likely(pool))
		lock_pool(pool);
	return pool;
}

static inline void put_worker_pool(struct worker_pool *pool)
{
	if (!pool_belong_syswq(pool)) {
		mutex_lock(&wq_pool_mutex);
		put_unbound_pool(pool);
		mutex_unlock(&wq_pool_mutex);
	}
}

static inline void put_work_pool_locked(struct worker_pool *pool)
{
	if (skp_likely(pool)) {
		unlock_pool(pool);
		put_worker_pool(pool);
	}
}

static void rcu_free_wq(struct rcu_head *ptr)
{
	struct workqueue_struct *wq = rcu2wq(ptr);
	if (wq->flags & WQ_UNBOUND) {
		free(wq->unbound_pwqs);
	} else {
		free_percpu(wq->cpu_pwqs);
	}
	log_info("free memory of wq: %p [%s], %s", wq, wq->flags & WQ_UNBOUND ?
		(wq->flags & __WQ_ORDER ? "order" : "unbound") : "bound", wq->name);
	free(wq);
}

static void rcu_free_pool(struct rcu_head *ptr)
{
	struct worker_pool *pool = rcu2wp(ptr);
	if (WQ_WARN_ON(!(pool->cpu < 0)) ||
		WQ_WARN_ON(!list_empty(&pool->worklist)) ||
		WQ_WARN_ON(pool_belong_syswq(pool)))
		return;
	log_info("free memory of worker pool : %p", pool);
	WRITE_ONCE(pool->flags, 0);
	free(pool);
}

static void init_pwq(struct pool_workqueue* pwq, struct workqueue_struct* wq,
		struct worker_pool* pool)
{
	WQ_BUG_ON((uintptr_t)pwq & WQ_WORK_FLAG_MASK);
	WQ_WARN(pool->refcnt < 1);
	pwq->pool = pool;
	pwq->wq = wq;
	pwq->nr_active = 0;
	pwq->refcnt = 1;
	pwq->nr_dispatch = 0;
	pwq->max_active = wq->saved_max_active;

	INIT_LIST_HEAD(&pwq->pwqs_node);
	INIT_LIST_HEAD(&pwq->delayed_works);

	INIT_WORK(&pwq->release_work, pwq_release_workfn);
	log_debug("initial queue pool %p [%p] of wq %p [%s]", pwq, pool,
		wq, wq->name);
}

static void pwq_adjust_max_active(struct pool_workqueue *pwq)
{
}

/*可以将其他的WQ的PWQ移动到自己的PWQ链表上进行管理？*/
static void link_pwq(struct pool_workqueue* pwq)
{
	struct workqueue_struct *wq = pwq->wq;
	WQ_BUG_ON(!mutex_is_locked(&wq->mutex));
	if (WQ_WARN_ON(!list_empty(&pwq->pwqs_node)))
		return;
	pwq_adjust_max_active(pwq);
	list_add(&pwq->pwqs_node, &wq->pwqs);
}

static void init_worker_pool(struct worker_pool *pool, int cpu, uint32_t flags)
{
	WQ_BUG_ON(cpu >= NR_CPUS);
	/*地址必须是 WQ_WORK_FLAG_BITS 对齐的*/
	WQ_BUG_ON((unsigned long)pool & WQ_WORK_FLAG_MASK);

	memset(pool, 0, sizeof(*pool));

	pool->cpu = cpu;
	pool->refcnt = 1;
	pool->flags = flags;
	pool->id = WQ_POOL_INVALID_ID;
	atomic_set(&pool->nr_running, 0);

	INIT_LIST_HEAD(&pool->worklist);
	INIT_LIST_HEAD(&pool->idle_worker);
	INIT_LIST_HEAD(&pool->workers);
	INIT_HLIST_NODE(&pool->hash_node);

	/*如果是绑定工作队列，则将关联的定时器也绑定在同一CPU上，减少竞争*/
	uev_timer_init(&pool->idle_timer, idle_worker_timeout);
	uev_timer_setcpu(&pool->idle_timer, cpu);

	pool_init_lock(pool);
	hash_init(pool->busy_worker);
	mutex_init(&pool->attach_mutex);

	log_debug("initial worker pool %p", pool);
}

static int worker_pool_assign_id(struct worker_pool *pool)
{
	int pid;

	WQ_BUG_ON(!mutex_is_locked(&wq_pool_mutex));

	pool_idr_writelock();
	pid = idr_alloc(&wq_pool_idr, pool);
	pool_idr_writeunlock();
	if (WARN_ON(pid < 0))
		return pid;
	WQ_BUG_ON(pid < WQ_POOL_MIN_ID);
	WQ_BUG_ON(pid > WQ_POOL_MAX_ID);
	pool->id = pid;
	return 0;
}

static inline void worker_leave_idle(struct wq_worker *worker)
{
	struct worker_pool *pool = worker->pool;
	if (WQ_WARN_ON(!(worker->flags & WORKER_IDLE)))
		return;
	pool->nr_idles--;
	list_del_init(&worker->entry);
	worker_clr_flags(worker, WORKER_IDLE);
	log_debug("worker %p leave idle on worker pool %p", worker, pool);
}

static void worker_enter_idle(struct wq_worker *worker)
{
	struct worker_pool *pool = worker->pool;

	if (WQ_WARN_ON(worker->flags & WORKER_IDLE) ||
			WQ_WARN_ON(!list_empty(&worker->entry) &&
			!(hlist_unhashed_careful(&worker->hentry))))
		return;

	pool->nr_idles++;
	worker->flags |= WORKER_IDLE;
	get_similar_timestamp(&worker->last_active);
	/*LIFO，栈模式提高 cache 命中*/
	list_add(&worker->entry, &pool->idle_worker);
	/*回收一些空闲的线程*/
	if (too_many_workers(pool))
		uev_timer_add(&pool->idle_timer, IDLE_WORKER_TIMEOUT);

	WQ_WARN(pool_belong_syswq(pool) &&
		pool->nr_workers == pool->nr_idles &&
		atomic_read(&pool->nr_running));

	log_debug("worker %p enter idle on worker pool %p", worker, pool);
}

/*必须从 idle 状态销毁*/
static void destroy_worker(struct wq_worker* worker)
{
	struct worker_pool *pool = worker->pool;

	WQ_BUG_ON(!pool_is_locked(pool));
	if (WQ_WARN_ON(worker->curr_work) ||
			WQ_WARN_ON(!list_empty(&worker->scheduled)) ||
			WQ_WARN_ON(!(worker->flags & WORKER_IDLE)))
		return;

	log_info("worker %p has been destroyed on pool %p[%d] [idle : %u, all : %u]",
			 worker, pool, pool->id, pool->nr_idles, pool->nr_workers);

	/*
	 * 如果是回收路径触发的销毁，
	 * 则必须保证回收后 idle大于1，否则可能造成 pool 释放路径的僵死
	 * @see put_unbound_pool()
	 */
	pool->nr_idles--;
	pool->nr_workers--;
	worker->flags |= WORKER_DIE;
	list_del_init(&worker->entry);
	/*唤醒，让线程自行销毁*/
	wake_up_one_locked(&worker->waitqueue);
}

static void worker_detach_from_pool(struct wq_worker *worker,
		struct worker_pool *pool)
{
	completion_t *detach_completion = NULL;

	mutex_lock(&pool->attach_mutex);
	list_del_init(&worker->node);
	static_mb();
	if (list_empty(&pool->workers))
		detach_completion = pool->detach_completion;
	mutex_unlock(&pool->attach_mutex);

	worker->flags &= ~ WORKER_UNBOUND;

	if (detach_completion)
		complete(detach_completion);

	log_debug("worker %p/%p detach to pool %p",
			  worker, worker->worker_thread.pthid, pool);
}

static void maybe_create_worker(struct worker_pool* pool)
{
restart:
	unlock_pool(pool);

	while (1) {
		if (create_worker(pool) || !need_to_create_worker(pool))
			break;

		log_debug("manage worker %p going to sleep on pool %p",
				  current_wq_worker(), pool);

		/*不能创建太频繁，休眠一会，此时worker 一定不能是 运行状态*/
		msleep_unintr(CREATE_COOLDOWN);
		/*休眠期间，有很多工作线程又处于了空闲状态*/
		if (!need_to_create_worker(pool))
			break;
	}

	lock_pool(pool);

	if (need_to_create_worker(pool))
		goto restart;
}

static bool manage_workers(struct wq_worker *worker)
{
	struct worker_pool *pool = worker->pool;
	if (pool->flags & POOL_MANAGER_ACTIVE)
		return false;

	pool->flags |= POOL_MANAGER_ACTIVE;
	pool->manager = worker;
	maybe_create_worker(pool);
	pool->manager = NULL;
	pool->flags &= ~ POOL_MANAGER_ACTIVE;

	if (waitqueue_active(&wq_manager_wait))
		wake_up_all(&wq_manager_wait);

	return true;
}

static void move_linked_works(struct work_struct *work, struct list_head *head)
{
	struct work_struct *n;
	/* 将 work 和其后接的所有 linked 类型的 work 都加入到指定的队列中，
	 * 直到搬移了下一个非 linked 的 work 后停止
	 * {work1 |barrier1_work1|barrier2_work1|...|barrier_work1}{work2 |....}
	 * |linked|   linked     |   linked     |...|  unlinked   | linked|
	 * 一组 linked work 必然以非 linked 结尾
	 * work 被放入的了自身的 scheduled 链表中
	 */
	list_for_each_entry_safe_from(work, n, NULL, entry) {
		list_move_tail(&work->entry, head);
		if (!(work->flags & WQ_WORK_LINKED))
			break;
	}
}

/* 激活指定的 延迟任务，
 * 将任务从 工作任务池队列 中搬移到 线程池队列中
 * 并将后续的 wq_barrier 一起搬移到线程池队列中*/
static inline void pwq_activate_delayed_work(struct pool_workqueue *pwq,
		struct work_struct *work)
{
	WQ_WARN(pwq != get_work_pwq(work));
	/*转移到 pool 中*/
	move_linked_works(work, &pwq->pool->worklist);
	__clear_bit(WQ_WORK_DELAYED_BIT, &work->flags);
	pwq->nr_active++;
}

/*激活第一个 延迟任务*/
static inline void pwq_activate_first_delayed(struct pool_workqueue *pwq)
{
	pwq_activate_delayed_work(pwq, first_delayedwork(pwq));
}

/*完成一个任务，激活后续延迟任务，并递减对所属 pwq 的引用计数*/
static inline void pwq_dec_nr_in_flight(struct pool_workqueue *pwq, bool barrier)
{
	uintptr_t rmvseq;
	struct workqueue_struct *wq = pwq->wq;

	if (skp_likely(!barrier))
		pwq->nr_active--;

	/*提交延迟的任务*/
	if (!list_empty(&pwq->delayed_works) && pwq->nr_active < pwq->max_active)
		pwq_activate_first_delayed(pwq);

	/*扩展符号位 process flusher?*/
	rmvseq = (uintptr_t)xadd(&wq->remove_seq, 1);
	/*wake up waiter*/
	if (waitqueue_active(&wq->wait_flusher))
		__wake_up_all(&wq->wait_flusher, (void*)(rmvseq + 1));

	/*释放应用之前，wq 一定是有效的*/
	put_pwq(pwq);
}

/* 分派了一个任务到 pwq ，递增 pwq 的引用计数*/
static inline void pwq_inc_nr_in_flight(struct pool_workqueue *pwq, bool active)
{
	WQ_BUG_ON(!get_pwq(pwq));
	if (active)
		pwq->nr_active++;
	xadd(&pwq->wq->insert_seq, 1);
}

/**查找正执行 work 的 worker 线程*/
static struct wq_worker *find_worker_executing_work(struct worker_pool *pool,
		struct work_struct *work)
{
	struct wq_worker *worker;
	BUILD_BUG_ON_NOT_POWER_OF_2(ARRAY_SIZE(pool->busy_worker));
	hash_for_each_possible(pool->busy_worker, worker, hentry, work) {
		/*找到 pool 可能正在运行相同 work 的 worker*/
		if (worker->curr_work==work && skp_likely(worker->curr_func==work->func))
			return worker;
	}
	return NULL;
}

static void process_one_work(struct wq_worker *worker, struct work_struct *work)
{
	bool barrier = false;
	struct worker_pool *pool = worker->pool;
	struct pool_workqueue *pwq = get_work_pwq(work);

	/* work 并发检查
	 * 1. 执行 __cancel_work() 后，并抹去的历史调度信息，
	 * 而后立马又执行 wq_queue_work_on()
	 * 并分配到同一个 wq_pool 上，但是被不同的工作线程立即执行，
	 * 触发执行后就会来到此路径
	 * 2. 在进入工作回调时就立马排队，而工作回调运行时间很长
	 * 主要需要整体移动，包括后面的 barrier 工作对象
	 */
	struct wq_worker *collision = find_worker_executing_work(pool, work);
	if (skp_unlikely(collision)) {
		move_linked_works(work, &collision->scheduled);
		return;
	}

	WQ_BUG_ON(!pwq || !pwq->refcnt);

	/*worker 处于忙状态
	 * 1. 将 pwq 的引用转移到 worker 上，相当于被 pool 引用
	 * 2. pwq 又引用 pool，制造暂时的环形引用
	 * 3. 所以除非执行完毕，否则他们中的任何一个都不会被销毁
	 */
	worker->curr_pwq = pwq;
	worker->curr_work = work;
	worker->curr_func = work->func;
	hash_add(pool->busy_worker, &worker->hentry, work);

	list_del_init(&work->entry);

	/*因为 unbound 类型的线程永远不会增减 nr_running
	 *所有 每次都会尽量的唤醒其他的 idle worker 处理后续的任务
	 *（如果有任务待处理）*/
	if (need_more_worker(pool))
		wake_up_worker(pool);

	barrier = !!(work->flags & WQ_WORK_BARRIER);

	/*执行后就一直使用 pool id 索引，并移除对 pwd 的引用
	 *然后再次执行时就会直接被分配到 pool 上*/
	set_work_pool_and_clear_pending(work, pool->id);

	work_start_process(work);

	pool->nr_triggers++;
	unlock_pool(pool);

	/* 执行work，之后work可能被销毁了，
	 * 可以使用其指针，但不能对其解引用
	 */
	worker->curr_func(work);

	lock_pool(pool);

	hash_del(&worker->hentry);
	worker->curr_work = NULL;
	worker->curr_func = NULL;
	worker->curr_pwq = NULL;

	/*释放 wq_queue_work() 时 获取的 pwq 的引用计数*/
	pwq_dec_nr_in_flight(pwq, barrier);
}

static void process_scheduled_works(struct wq_worker *worker)
{
	while (!list_empty(&worker->scheduled)) {
		process_one_work(worker, first_scheduledwork(worker));
	}
}

static int worker_cb(void *arg)
{
	sigset_t sigset;
	DEFINE_WAITQUEUE(wait);
	struct work_struct *work;
	struct worker_pool *pool = arg;
	struct wq_worker *worker = current_wq_worker();

	BUG_ON(!worker);
	BUG_ON(worker->pool != pool);

	signal_block_all(&sigset);

	if (pool->cpu != -1)
		thread_bind(pool->cpu);

woke_up:
	lock_pool(pool);

	remove_wait_queue_locked(&worker->waitqueue, &wait);

	if (skp_unlikely(worker->flags & WORKER_DIE))
		goto out;

	/*清除idle状态，并从idle链表中移除*/
	worker_leave_idle(worker);
recheck:
	/*检查是否有任务需要处理且没有一个活动线程*/
	if (!need_more_worker(pool))
		goto sleep;

	/*manage worker?*/
	/* 如果没有任何空闲线程，那么该工作线程就临时转变为管理线程去
	 * 创建一些线程，并暂时不处理工作任务*/
	if (skp_unlikely(!may_start_working(pool)) && manage_workers(worker))
	/*有解锁，排队任务可能已被处理了*/
		goto recheck;

	WQ_WARN(!list_empty(&worker->scheduled));

	/*增加并发度，清除准备状态*/
	worker_clr_flags(worker, WORKER_PREP);

	do {
		work = workerpool_firstwork(pool);
		/*开始处理任务*/
		if (skp_likely(!(work->flags & WQ_WORK_LINKED))) {
			/*处理的任务休眠，会降低并发度，但是不会改变运行状态*/
			process_one_work(worker, work);
			/*处理过程中，插入了调度任务，barrier work*/
			if (skp_unlikely(!list_empty(&worker->scheduled)))
				process_scheduled_works(worker);
		} else {
			/*这个任务后续都是调度任务，需要一次性执行完毕，barrier work*/
			move_linked_works(work, &worker->scheduled);
			process_scheduled_works(worker);
		}
		/* 如果执行任务期间休眠，那么会唤醒其他线程来处理任务，
		 * 当此线程执行完任务后，发现有其他活动线程了，则退出循环
		 * 非绑定线程没有并发度问题？会一直运行？
		 */
	} while (keep_working(pool));

	/*降低并发度，设置准备状态*/
	worker_set_flags(worker, WORKER_PREP);

sleep:
	worker_enter_idle(worker);

	add_wait_queue_exclusive_locked(&worker->waitqueue, &wait);
	unlock_pool(pool);

	/*休眠，回收缓存的资源*/
	/*回收通用内存和页缓存*/
	radix_tree_reclaim();
	umem_cache_reclaim();

	wait_on(&wait);

	log_debug("worker %p has been waked up on %p [%u]",
			  worker, pool, atomic_read(&pool->nr_running));
	/*wait notify*/
	goto woke_up;

out:
	unlock_pool(pool);
	worker_detach_from_pool(worker, pool);
	signal_unblock_all(&sigset);
	/*剥离自己，自己释放线程的资源*/
	uthread_detach();
	return 0;
}

static void worker_attach_to_pool(struct wq_worker *worker,
		struct worker_pool *pool)
{
	mutex_lock(&pool->attach_mutex);
	if (!pool_belong_syswq(pool))
		worker->flags |= WORKER_UNBOUND;
	list_add_tail(&worker->node, &pool->workers);
	mutex_unlock(&pool->attach_mutex);
	log_debug("worker %p/%p attach to pool %p",
			  worker, worker->worker_thread.pthid, pool);
}

/**为线程池创建第一个线程*/
static struct wq_worker *create_worker(struct worker_pool *pool)
{
	struct wq_worker *worker = (void*)__uthread_create(
			worker_cb, pool, sizeof(*worker));
	if (skp_unlikely(!worker))
		return NULL;

	worker->pool = pool;
	worker->curr_pwq = NULL;
	worker->curr_work = NULL;
	worker->curr_func = NULL;

	INIT_LIST_HEAD(&worker->node);
	INIT_LIST_HEAD(&worker->entry);
	INIT_LIST_HEAD(&worker->scheduled);

	worker->flags = WORKER_PREP;
	worker->last_active.tv_sec = 0;
	worker->last_active.tv_nsec = 0;
	init_waitqueue_head(&worker->waitqueue);
	__set_bit(THREAD_ISQUEUEWORKER_BIT, &worker->worker_thread.flags);

	/*连接到池中进行管理*/
	worker_attach_to_pool(worker, pool);

	/*launch worker*/
	lock_pool(pool);
	pool->nr_workers++;

	log_info("worker [%p] has been created on pool [%p/%u] : "
			 "idle [%u], workers [%u]", worker, pool, pool->id,
			 pool->nr_idles, pool->nr_workers);

	/*进入 idle 模式，与 worker_thread_cb() 匹配*/
	worker_enter_idle(worker);
	/*锁内唤醒？*/
	BUG_ON(uthread_wakeup(&worker->worker_thread));
	unlock_pool(pool);

	return worker;
}

static void put_unbound_pool(struct worker_pool *pool)
{
	uint32_t pool_id;
	struct wq_worker *worker;
	DEFINE_COMPLETION(detach_completion);
	WQ_BUG_ON(!mutex_is_locked(&wq_pool_mutex));

	if (skp_likely(--pool->refcnt))
		return;

	if (WQ_WARN_ON(!(pool->cpu < 0)) ||
		WQ_WARN_ON(!list_empty(&pool->worklist)) ||
		WQ_WARN_ON(pool_belong_syswq(pool)))
		return;

	WARN_ON(in_atomic());
	log_info("prepare release worker pool %p id %u", pool, pool->id);

	/* release id and unhash
	 * 将ID释放提前，这样其他路径就不会再次通过任务的历史信息
	 * 找此即将释放的工作线程池
	 */
	smp_wmb();
	pool_id = xchg(&pool->id, WQ_POOL_INVALID_ID);
	if (skp_likely(pool_id != WQ_POOL_INVALID_ID)) {
		pool_idr_writelock();
		WQ_BUG_ON(idr_remove(&wq_pool_idr, pool_id) != pool);
		pool_idr_writeunlock();
	}

	hash_del(&pool->hash_node);

	lock_pool(pool);
	/*当前 pool 有工作线程 进入了管理模式*/
	__wait_event(&wq_manager_wait, !(pool->flags & POOL_MANAGER_ACTIVE), false,
		({
			unlock_pool(pool);
			wait_on_timeout(&WAIT,1000);
			lock_pool(pool);
			1;
		}));
	/*阻止再次进入管理模式*/
	pool->flags |= POOL_MANAGER_ACTIVE;

	/*杀死所有空闲的线程*/
	while ((worker = first_idle_worker(pool)))
		destroy_worker(worker);
	unlock_pool(pool);

	/*
	 * 释放锁后，僵死的工作线程被唤醒
	 * 最后一个工作先退出时，负责唤醒本路径等待
	 * @see worker_detach_from_pool()
	 */
	mutex_lock(&pool->attach_mutex);
	if (!list_empty(&pool->workers))
		pool->detach_completion = &detach_completion;
	mutex_unlock(&pool->attach_mutex);

	/*等待最后一个空闲线程退出*/
	if (pool->detach_completion)
		wait_for_completion(pool->detach_completion);

	/*不应该还有活动线程存在*/
	WQ_WARN(pool->nr_workers || pool->nr_idles);
	/*同步删除定时器*/
	uev_timer_delete_sync(&pool->idle_timer);

	log_debug("release success worker pool %p id %u", pool, pool->id);

	log_info("unbound worker pool [%p/%u] statistic : "
		"process works [%lu]", pool, pool->id, pool->nr_triggers);

	/*延迟释放*/
	call_rcu_sched(&pool->rcu, rcu_free_pool);
}

static struct worker_pool *get_unbound_pool(struct pool_workqueue *pwq)
{
	struct worker_pool *pool;
	WQ_BUG_ON(!mutex_is_locked(&wq_pool_mutex));

	/*从池中获取，如果哈希后没有，则创建*/
	hash_for_each_possible(unbound_pool_hash, pool, hash_node, pwq) {
		if (WARN_ON(pool->refcnt++ <= 0))
			return NULL;
		return pool;
	}

	pool = aligned_alloc(WQ_ALIGNED_SIZE, sizeof(*pool));
	if (skp_unlikely(!pool))
		return NULL;

	init_worker_pool(pool, WQ_WORK_CPU_UNBOUND, POOL_DISASSOCIATED);
	/*为 pool 分配赋值一个 id*/
	if (skp_unlikely(worker_pool_assign_id(pool))) {
		log_warn("too many pool has been created");
		goto fail;
	}

	log_debug("create unbound worker pool : %p id : %u", pool, pool->id);

	if (READ_ONCE(wq_online) && !create_worker(pool))
		goto fail;

	hash_add(unbound_pool_hash, &pool->hash_node, pwq);

	return pool;
fail:
	put_unbound_pool(pool);
	return NULL;
}

static int alloc_and_link_pwqs(struct workqueue_struct *wq)
{
	bool highpri = 0;
	struct worker_pool *pool;
	struct pool_workqueue *pwq;
	int cpu, i = 0, nr_pwq;

#ifdef WORKQUEUE_HAVE_HIGHPRI
	highpri = !!(wq->flags & WQ_HIGHPRI);
#endif

	if (!(wq->flags & WQ_UNBOUND)) {
		/*绑定类型的 wq 共享全局静态的 worker_pool*/
		log_debug("prepare alloc queue pool for bound wq : %p[%s]", wq, wq->name);

		/*
		 * 1. wq 下有 per-cpu 的 pool_workqueue
		 * 2. 每个 pool_workqueue 与 per-cpu 的 worker_pool 连接
		 * 3. 每个 worker_pool 有一个或多个 worker
		 * 4. 这样可以实现从 wq 排队到指定的CPU上运行 work
		 */
		wq->cpu_pwqs = alloc_percpu(struct pool_workqueue);
		if (skp_unlikely(!wq->cpu_pwqs))
			return -ENOMEM;

		/*将队列池绑定到线程池上，队列池的一致性也由线程池的锁来保证*/
		mutex_lock(&wq_pool_mutex);
		for_each_possible_cpu(cpu) {
			pwq = per_cpu_ptr(wq->cpu_pwqs, cpu);
			pool = &per_cpu(cpu_wq_worker_pools, cpu)[highpri];
			pool->refcnt++;
			init_pwq(pwq, wq, pool);
			mutex_lock(&wq->mutex);
			link_pwq(pwq);
			mutex_unlock(&wq->mutex);
		}
		mutex_unlock(&wq_pool_mutex);

		return 0;
	}

	/*非绑定类型的使用少于 NR_CPU 个数量的 pool_workqueue 来模拟 node*/
	BUILD_BUG_ON(!WQ_UNBOUND_PWQS);

	/*非绑定类型的 工作队列 的 队列池为 CPU 核数的一半*/
	nr_pwq = WQ_UNBOUND_PWQS;
	if (wq->flags & __WQ_ORDER) {
		/*顺序工作队列，直到上一个调度完成，每次排队只有一个工作会被调度*/
		nr_pwq = 1;
		wq->saved_max_active = 1;
		log_debug("prepare alloc queue pool for order wq : %p[%s]",
			wq, wq->name);
	} else {
		log_debug("prepare alloc queue pool for unbound wq : %p[%s]",
			wq, wq->name);
	}

	/*shared unbound dynamic pool*/
	wq->unbound_pwqs = aligned_alloc(WQ_ALIGNED_SIZE,
							nr_pwq * sizeof(struct pool_workqueue));
	if (skp_unlikely(!wq->unbound_pwqs))
		return -ENOMEM;

	for (i = 0; i < nr_pwq; i++) {
		pwq = &wq->unbound_pwqs[i];
		/*从共享的 worker_pool 池中获取一个*/
		mutex_lock(&wq_pool_mutex);
		pool = get_unbound_pool(pwq);
		if (skp_unlikely(!pool))
			goto fail;
		mutex_unlock(&wq_pool_mutex);
		/*inc ref of pool*/
		init_pwq(pwq, wq, pool);
		mutex_lock(&wq->mutex);
		link_pwq(pwq);
		mutex_unlock(&wq->mutex);
	}

	return 0;
fail:
	/*becareful*/
	while (i-- > 0) {
		pwq = &wq->unbound_pwqs[i];
		put_unbound_pool(pwq->pool);
	}

	mutex_unlock(&wq_pool_mutex);

	free(wq->unbound_pwqs);

	return -ENOMEM;
}

static void pwq_release_workfn(struct work_struct* worker)
{
	bool islast = false;
	struct pool_workqueue *pwq = relwork2pwq(worker);
	struct worker_pool *pool = pwq->pool;
	struct workqueue_struct *wq = pwq->wq;

	if (WQ_WARN_ON(pwq_belong_syswq(pwq)))
		return;

	log_debug("release queue pool %p [%p] on wq %p", pwq, pool, wq);

	mutex_lock(&wq->mutex);
	list_del_init(&pwq->pwqs_node);
	/*workqueue 与 pwq 联动计数*/
	islast = list_empty(&wq->pwqs);
	mutex_unlock(&wq->mutex);

	mutex_lock(&wq_pool_mutex);
	put_unbound_pool(pool);
	mutex_unlock(&wq_pool_mutex);

	if (skp_likely(!islast))
		return;

	log_debug("wq have not any queue pool, prepare to free wq : %p", wq);
	call_rcu_sched(&wq->rcu, rcu_free_wq);
}

static void idle_worker_timeout(struct uev_timer* timer)
{
	uint64_t now = similar_abstime(NULL, 0);
	struct worker_pool *pool = timer2wp(timer);

	lock_pool(pool);
	while (too_many_workers(pool)) {
		int32_t expire;
		uint64_t future;
		struct timespec last_active;
		struct wq_worker * worker = workerpool_lastidle(pool);

		if (WARN_ON(list_empty(&pool->idle_worker)))
			break;

		last_active = worker->last_active;
		future = timestamp_offset(&last_active, IDLE_WORKER_TIMEOUT);
		expire = (int32_t)((future - now)/1000000);

		/*虽然超时，但是最后一次活动时间到现在还未达到回收时间*/
		if (future > now && expire) {
			log_debug("last worker %p of pool %p trigger timer on future again:"
					  " %d(%lld,%lld)", worker, pool, expire, future, now);
			uev_timer_modify(&pool->idle_timer, expire);
			break;
		}
		log_info("prepare recover idle worker %p on pool : %p, %u",
			worker, pool, pool->nr_workers);
		destroy_worker(worker);
	}
	unlock_pool(pool);
}


/**
 * 初始化数据结构，还没有启动线程
 * 1. worker_pool 的ID分配器
 * 2. 静态的 worker_pool
 */
static void workqueue_init_early(void)
{
	int cpu;
	struct worker_pool *pool;

	/*预分配 radix_tree 的线程私有缓存*/
	radix_tree_preload();
	/*最后一个ID 不使用，用于标记无效的ID*/
	WQ_BUG_ON(idr_init_base(&wq_pool_idr, WQ_POOL_MIN_ID, WQ_POOL_MAX_ID));

	/*初始化静态线程池*/
	for_each_possible_cpu(cpu) {
		for_each_cpu_worker_pool(pool, cpu) {
			/*静态的 pool 支持绑定*/
			init_worker_pool(pool, cpu, 0);
			/*分配ID*/
			mutex_lock(&wq_pool_mutex);
			WQ_BUG_ON(worker_pool_assign_id(pool));
			mutex_unlock(&wq_pool_mutex);
			log_debug("create bound worker pool : %p [%d] id : %u",
				pool, cpu, pool->id);
		}
	}

	/*创建系统默认的 workqueue*/
	system_wq = __alloc_workqueue("events", 0, 0);
	system_highpri_wq = __alloc_workqueue("events_highpri", WQ_HIGHPRI, 0);
	system_long_wq = __alloc_workqueue("events_long", 0, 0);
	system_unbound_wq = __alloc_workqueue("events_unbound", WQ_UNBOUND,
							WQ_UNBOUND_MAX_ACTIVE);

	WQ_BUG_ON(!system_wq || !system_highpri_wq ||
			  !system_long_wq || !system_unbound_wq);
}

struct workqueue_struct *
__alloc_workqueue(const char *fmt, uint32_t flags, int32_t max_active, ...)
{
	va_list args;
	uint32_t oflags = flags;
	struct workqueue_struct *wq;
	struct pool_workqueue *pwq;

	flags &= (WQ_HIGHPRI | WQ_UNBOUND);
	WARN_ON(flags != oflags);

	wq = malloc(sizeof(*wq));
	if (skp_unlikely(!wq))
		return NULL;

	memset(wq, 0, sizeof(*wq));

	va_start(args, max_active);
	vsnprintf(wq->name, sizeof(wq->name), fmt, args);
	va_end(args);

	/*max 为 1 且创建的是 非绑定CPU的工作队列则为顺序队列*/
	if (max_active == 1 && (flags & WQ_UNBOUND))
		flags |= __WQ_ORDER;

	max_active = max_active ?: WQ_DFL_ACTIVE;
	max_active = clamp_t(int32_t, max_active, 1, WQ_MAX_ACTIVE);

	wq->flags = flags;
	wq->saved_max_active = max_active;

	mutex_init(&wq->mutex);
	INIT_LIST_HEAD(&wq->list);
	INIT_LIST_HEAD(&wq->pwqs);
	init_waitqueue_head(&wq->wait_flusher);

	/*根据标志分配独立的或链接共享的队列池*/
	if (alloc_and_link_pwqs(wq))
		goto err_free;

	/*链接到全局链表中进行管理*/
	mutex_lock(&wq_pool_mutex);

	mutex_lock(&wq->mutex);
	for_each_pwq(pwq, wq)
		pwq_adjust_max_active(pwq);
	mutex_unlock(&wq->mutex);

	list_add_tail(&wq->list, &workqueues);
	mutex_unlock(&wq_pool_mutex);

	log_debug("create wq success : %p [%s]", wq, wq->name);

	return wq;
err_free:
	free(wq);
	return NULL;
}

void destroy_workqueue(struct workqueue_struct *wq)
{
	LIST__HEAD(pwqs_list);
	uint32_t drain_cnt = 0;
	struct pool_workqueue *pwq, *next;

	if (WARN_ON(wq_belong_syswq(wq))) {
		drain_workqueue(wq);
		return;
	}

	mutex_lock(&wq->mutex);
	if (WARN_ON(wq->flags & __WQ_DESTROYING)) {
		mutex_unlock(&wq->mutex);
		return;
	}
	wq->flags |= __WQ_DESTROYING;
	mutex_unlock(&wq->mutex);

	log_debug("prepare destroy wq : %p [%s]", wq, wq->name);
redrain:
	/*等待所有排队的、运行中的工作完成*/
	drain_workqueue(wq);

	/*遍历工作队列查看是否还有工作未完成*/
	mutex_lock(&wq->mutex);
	for_each_pwq_safe(pwq, next, wq) {
		bool drained;

		/*共享他人的 pwq ?*/
		if (WARN_ON(pwq->wq != wq)) {
			/*将不属于自己的PWQ 清除？*/
			list_move(&pwq->pwqs_node, &pwqs_list);
			continue;
		}

		lock_pool(pwq->pool);
		drained = !(pwq->refcnt > 1 || pwq->nr_active ||
					!list_empty(&pwq->delayed_works));
		unlock_pool(pwq->pool);

		/*
		 *判断是否在其他排队的任务全部已经完成，
		 *如果没有完成需要再次 drain_workqueue():
		 *1. 有路径引用了其中的队列池
		 *2. 有活动的或排队的任务
		 *3. 有延迟排队的任务
		 */
		if (drained)
			continue;

		if (++drain_cnt == 10 || drain_cnt % 100 == 0)
			log_warn("destroy workqueue : %s "
				"isn't complete after %u tries", wq->name, drain_cnt);
		mutex_unlock(&wq->mutex);
		goto redrain;
	}
	mutex_unlock(&wq->mutex);

	/*将不属于自己的 PWQ 归还？*/
	list_for_each_entry_safe(pwq, next, &pwqs_list, pwqs_node) {
		struct workqueue_struct *owq = pwq->wq;
		mutex_lock(&owq->mutex);
		list_move(&pwq->pwqs_node, &owq->pwqs);
		if (owq->flags & __WQ_DESTROYING)
			put_pwq_unlocked(pwq);
		/*如果 put_pwq() 造成释放也没有问题，
		 *因为异步释放一定会等待这个 wq.mutex 的释放
		 * worker_pool 也是一样
		 */
		mutex_unlock(&owq->mutex);
	}

	/*剥离 wq ，从全局看不到此 wq*/
	mutex_lock(&wq_pool_mutex);
	list_del_init(&wq->list);
	mutex_unlock(&wq_pool_mutex);

	/*递减 pwq 的引用计数，同时也是 wq 的引用计数，进行异步释放*/
	mutex_lock(&wq->mutex);
	for_each_pwq(pwq, wq) {
		put_pwq_unlocked(pwq);
	}
	mutex_unlock(&wq->mutex);

	log_debug("destroy success wq : %p [%s]", wq, wq->name);
	/*不会直接释放内存，需要 rcu_free */
}

static int wfl_wakefn(wait_queue_t *wait, void *key)
{
	/*注意要扩展符号位*/
	int16_t remove_seq = (int16_t)(long)key;
	struct wfl_wait *fwait = container_of(wait, struct wfl_wait, wait);

	/*有差值表示尚有排队的任务没有运行完成，继续等待*/
	if (remove_seq != fwait->insert_seq)
		return 0;
	fwait->remove_seq = remove_seq;
	log_info("wake up flusher waiting for insert seq : %d, "
		"current remove seq : %d", fwait->insert_seq, remove_seq);
	return autoremove_wake_function(wait, key);
}

void flush_workqueue(struct workqueue_struct *wq)
{
	/*获取插入序号快照*/
	struct wfl_wait fwait;

	if (WARN_ON(!wq_online))
		return;

	init_waitqueue_entry(&fwait.wait);
	fwait.wait.func = wfl_wakefn;

	wait_queue_head_lock(&wq->wait_flusher);

	fwait.insert_seq = READ_ONCE(wq->insert_seq);
	fwait.remove_seq = READ_ONCE(wq->remove_seq);
	static_mb();

	add_wait_queue_locked(&wq->wait_flusher, &fwait.wait);
	log_debug("launch waiting for insert seq : %d, current remove seq : %d",
		fwait.insert_seq, fwait.remove_seq);
	while (fwait.insert_seq != fwait.remove_seq) {
		wait_queue_head_unlock(&wq->wait_flusher);
		wait_on(&fwait.wait);
		wait_queue_head_lock(&wq->wait_flusher);
	}
	log_debug("finish waiting for insert seq : %d, current remove seq : %d",
		fwait.insert_seq, fwait.remove_seq);

	remove_wait_queue_locked(&wq->wait_flusher, &fwait.wait);
	wait_queue_head_unlock(&wq->wait_flusher);
}

void drain_workqueue(struct workqueue_struct *wq)
{
	uint32_t flush_cnt = 0;
	struct pool_workqueue *pwq;

	if (WARN_ON(!wq_online))
		return;

	mutex_lock(&wq->mutex);
	if (!wq->nr_drainers++)
		wq->flags |= __WQ_DRAINING;
	mutex_unlock(&wq->mutex);

	log_debug("prepare drain wq : %p [%s]", wq, wq->name);

reflush:
	/*等待当前时刻的已排队任务完成*/
	flush_workqueue(wq);

	/*查看是否仍有任务待调度*/
	mutex_lock(&wq->mutex);
	for_each_pwq(pwq, wq) {
		bool drained;
		lock_pool(pwq->pool);
		/*没有已排队或未排队的任务则表示已清空*/
		drained = !pwq->nr_active && list_empty(&pwq->delayed_works);
		unlock_pool(pwq->pool);
		if (drained)
			continue;

		if (++flush_cnt == 10 || flush_cnt % 100 == 0)
			log_warn("drain workqueue : %s isn't complete after %u tries",
				wq->name, flush_cnt);

		mutex_unlock(&wq->mutex);
		goto reflush;
	}

	log_debug("drain success wq : %p [%s]", wq, wq->name);

	if (!--wq->nr_drainers)
		wq->flags &= ~ __WQ_DRAINING;
	mutex_unlock(&wq->mutex);

}

uint32_t workqueue_congested(struct workqueue_struct *wq)
{
	uint32_t nr_pwq = 0;
	struct pool_workqueue *pwq;

	mutex_lock(&wq->mutex);
	for_each_pwq(pwq, wq) {
		/*一定要是用 list_empty_careful()
		 *因 wq->mutex 并不是保护 pwq->delayed_works
		 */
		if (!list_empty_careful(&pwq->delayed_works))
			nr_pwq++;
	}
	mutex_unlock(&wq->mutex);

	return nr_pwq;
}

static void workqueue_release(void)
{
	LIST__HEAD(user_wq);
	int cpu;
	struct worker_pool *pool;
	struct workqueue_struct *wq, *next;
	if (!READ_ONCE(wq_online))
		return;

	log_info("stopping workqueue system");

	BUG_ON(current_wq_worker());

	mutex_lock(&wq_pool_mutex);
	list_for_each_entry_safe(wq, next, &workqueues, list) {
		/*移动到临时链表中去销毁*/
		if (WARN_ON(!wq_belong_syswq(wq)))
			list_move(&wq->list, &user_wq);
	}
	mutex_unlock(&wq_pool_mutex);

	list_for_each_entry_safe(wq, next, &user_wq, list) {
		destroy_workqueue(wq);
	}

	mutex_lock(&wq_pool_mutex);
	list_for_each_entry_safe(wq, next, &workqueues, list) {
		mutex_lock(&wq->mutex);
		wq->flags |= __WQ_DESTROYING;
		mutex_unlock(&wq->mutex);
	}
	mutex_unlock(&wq_pool_mutex);

	drain_workqueue(system_unbound_wq);
	drain_workqueue(system_long_wq);
	drain_workqueue(system_highpri_wq);
	drain_workqueue(system_wq);

	mutex_lock(&wq_pool_mutex);
	list_for_each_entry_safe(wq, next, &workqueues, list) {
		WARN_ON(!wq_belong_syswq(wq));
	}
	mutex_unlock(&wq_pool_mutex);

	for_each_possible_cpu(cpu) {
		for_each_cpu_worker_pool(pool, cpu) {
			log_info("system worker pool [%p/%u] statistic : "
				"process works [%lu]", pool,
				pool->id, pool->nr_triggers);
		}
	}
}

void __workqueue_init(bool single)
{
	int cpu, bkt;
	struct worker_pool *pool;

	/*依赖 时间系统 需要在其前初始化*/
	sysevent_init(single);

	big_lock();
	if (READ_ONCE(wq_online)) {
		big_unlock();
		return;
	}

	log_info("initialize workqueue system");

	/*开始初始化静态信息*/
	workqueue_init_early();

	/*到此所有静态信息已初始化完毕，可以统一创建线程了*/

	mutex_lock(&wq_pool_mutex);

	/*为静态创建的 worker_pool 创建线程*/
	for_each_possible_cpu(cpu) {
		for_each_cpu_worker_pool(pool, cpu) {
			pool->flags &= ~ POOL_DISASSOCIATED;
			BUG_ON(!create_worker(pool));
		}
	}

	/*为动态创建的 worker_pool 创建线程*/
	hash_for_each(unbound_pool_hash, bkt, pool, hash_node)
		BUG_ON(!create_worker(pool));

	mutex_unlock(&wq_pool_mutex);

	WRITE_ONCE(wq_online, true);
	atexit(workqueue_release);

	big_unlock();
}

static void insert_work(struct pool_workqueue *pwq, struct work_struct *work,
		struct list_head *head, unsigned long flags)
{
	struct worker_pool *pool = pwq->pool;
	/*判断是否一个延迟或内部work，如果是将不会增加排队数*/
	bool isactive = !(flags & (WQ_WORK_BARRIER | WQ_WORK_DELAYED));

	pwq_inc_nr_in_flight(pwq, isactive);
	set_work_pwq(work, pwq, flags);
	/*排队到末尾*/
	list_add_tail(&work->entry, head);

	/*
	 * 确保 wq_worker_sleeping() 查看到上面的改动
	 * Ensure either wq_worker_sleeping() sees the above
	 * list_add_tail() or we see zero nr_running to avoid workers lying
	 * around lazily while there are works to be processed.
	 */
	smp_mb();
	if (__need_more_worker(pool))
		wake_up_worker(pool);

	work_finish_sched(work);
}

static inline uint32_t hash_work(struct work_struct *work, const int bits)
{
#if 0
	return jhash_2words(((uintptr_t)work) & U32_MAX,
			(((uintptr_t)work) >> 32), __LINE__) & ((1U << bits) - 1);
#else
	return hash_ptr(work, bits);
#endif
}

static __always_inline struct pool_workqueue * select_pwq(int cpu,
		struct workqueue_struct *wq, struct work_struct *work)
{
	struct pool_workqueue *pwq;

	work_start_sched(work);

	/*在非工作线程中排队任务到正在清理工作任务的队列上是不允许的*/
	if (skp_unlikely(wq->flags & __WQ_DRAINING) &&
			WARN_ON(!is_chained_work(wq))) {
		return NULL;
	} else if (WARN_ON(wq->flags & __WQ_DESTROYING)) {
		/*正在被销毁*/
		return NULL;
	} else if (!(wq->flags & WQ_UNBOUND)) {
		/*绑定CPU的工作队列，均匀分配*/
		BUILD_BUG_ON_NOT_POWER_OF_2(NR_CPUS);
		if (cpu < 0 || skp_unlikely(cpu >= NR_CPUS)) {
			int hint = thread_cpu();
			cpu = hint>-1?(hint&(NR_CPUS-1)):hash_work(work,NR_CPUS_SHIFT);
		}
		pwq = per_cpu_ptr(wq->cpu_pwqs, cpu);
	} else if (wq->flags & __WQ_ORDER) {
		/*顺序队列，只有一个 pwq*/
		pwq = &wq->unbound_pwqs[0];
	} else {
		/*非绑定CPU的工作队列，均匀分配*/
		BUILD_BUG_ON_NOT_POWER_OF_2(WQ_UNBOUND_PWQS);
		pwq = &wq->unbound_pwqs[hash_work(work, ilog2(WQ_UNBOUND_PWQS))];
	}

	/*正在被释放，与 work 关联的 pwq 在工作线程中处理完成后可能会被异步释放
	 * @see pwq_dec_nr_in_flight()*/
	return pwq;
}

static bool __queue_work(int cpu, struct workqueue_struct *wq,
		struct work_struct *work)
{
	unsigned long workflags = 0;
	struct wq_worker *worker;
	struct list_head *worklist;
	struct pool_workqueue *pwq;
	struct worker_pool *last_pool;

	/*如果已经排队过，且没有指定CPU，那么每次排队都会有一个不同的pwq*/
	pwq = select_pwq(cpu, wq, work);
	if (skp_unlikely(!pwq))
		goto fail;

	last_pool = get_work_pool_and_lock(work);
	if (skp_likely(last_pool)) {
		if (skp_unlikely(last_pool != pwq->pool)) {
			/*1. 正在运行的任务 或 以前运行过的任务，使用 pool 标识
			 *2. 正在排队的任务，即将运行的任务，使用 pwq 标识
			 *3. 即使上面两种情况满足，但和本次指定了不同的 workqueue
			 * 则使用本次指定的，否则使用已标记的
			 */
			worker = find_worker_executing_work(last_pool, work);
			if (worker && skp_likely(worker->curr_pwq->wq == wq)) {
				/*4. 为了提高 cacheline 的命中，尽量使用同一个 worker 来调度 work*/
				pwq = worker->curr_pwq;
			} else {
				put_work_pool_locked(last_pool);
				/*4. 但不包括 目标 wq 改变的情况，使用本次原期望的 pwq*/
				last_pool = get_worker_pool_and_lock(pwq->pool);
			}
		}
	} else {
		last_pool = get_worker_pool_and_lock(pwq->pool);
	}

	if (WARN_ON(!last_pool || !pwq->refcnt)) {
		log_warn("workqueue has been destroyed : %p [%s]", wq, wq->name);
		goto fail;
	}

	if (WQ_WARN_ON(!list_empty(&work->entry))) {
		log_warn("work has been dispatched without taking PENDING: %p", work);
		goto out;
	}

	pwq->nr_dispatch++;
	/*检查队列是否满了*/
	if (skp_likely(pwq->nr_active < pwq->max_active)) {
		worklist = &pwq->pool->worklist;
	} else {
		workflags |= WQ_WORK_DELAYED;
		worklist = &pwq->delayed_works;
	}

	insert_work(pwq, work, worklist, workflags);

out:
	put_work_pool_locked(last_pool);
	return true;
fail:
	clear_bit(WQ_WORK_PENDING_BIT, &work->flags);
	return false;
}

bool queue_work_on(int cpu, struct workqueue_struct *wq, struct work_struct *work)
{
	if (WARN_ON(!wq))
		return false;
	if (!test_and_set_bit(WQ_WORK_PENDING_BIT, &work->flags))
		return __queue_work(cpu, wq, work);
	return false;
}

/**
 * work_busy - test whether a work is currently pending or running
 * @work: the work to be tested
 *
 * Test whether @work is currently pending or running.  There is no
 * synchronization around this function and the test result is
 * unreliable and only useful as advisory hints or for debugging.
 *
 * Return:
 * OR'd bitmask of WORK_BUSY_* bits.
 */
uint32_t work_busy(struct work_struct *work)
{
	uint32_t rc = 0;
	struct wq_worker *worker;
	struct worker_pool *pool;
	if (work_pending(work))
		rc |= WORK_BUSY_PENDING;

	pool = get_work_pool_and_lock(work);
	if (pool) {
		worker = find_worker_executing_work(pool, work);
		if (worker)
			rc |= WORK_BUSY_RUNNING;
		put_work_pool_locked(pool);
	}
	return rc;
}

static int cwt_wakefn(wait_queue_t *wait, void *key)
{
	struct cwt_wait *cwait = container_of(wait, struct cwt_wait, wait);

	/*使用Key比较，防止惊群效应*/
	if (cwait->work != key)
		return 0;
	return autoremove_wake_function(wait, key);
}
/**
 * 返回 1 表示从(定时器)排队状态回收，成功抢占了 pending
 * 返回 0 表示从执行或未排队状态回收，成功抢占了 pending
 * 返回 负数 表示需要再次尝试，其他路径也可能在执行 pending 状态回收
 */
static int try_to_grab_pending(struct work_struct *work, bool is_dwork)
{
	bool grabbed = false;
	struct worker_pool *pool;
	struct pool_workqueue *pwq;

	/*延迟工作任务*/
	if (is_dwork) {
		struct delayed_work *dwork = to_delayed_work(work);
		int32_t remain = uev_timer_delete(&dwork->timer, false);
		/* 因为 pending 不仅标识排队work 也标识排队内部的 timer
		 * 成功从定时器排队状态回收，一定没有清除过 pending
		 * 否则有以下两种情况
		 * 1. 可能没有排队
		 * 2. 或正在执行定时器回调
		 */
		if (skp_likely(remain > 0))
			return 1;
	}
	/*
	 * 到此处标志 work 的状态未知，需要 抢占 pending
	 * 如果成功则从即将执行的状态回收
	 * 1. 抢先延迟任务的定时器回调排队任务
	 * 2. 在任务回调执行中
	 */
	if (!test_and_set_bit(WQ_WORK_PENDING_BIT, &work->flags))
		return 0;

	pool = get_work_pool_and_lock(work);
	/*pool 为空，正在排队中，或正在被其他路径取消，或者从来未曾排队*/
	if (skp_unlikely(!pool))
		goto fail;
	pwq = get_pwq(get_work_pwq(work));
	/*如果 pwq 为空，则表明已经准备（可能处于 worker->scheduled 链表中
	 *或正在运行，需要再次尝试，或正在被取消，可能需要等待*/
	if (pwq && skp_likely(pwq->pool == pool)) {
		/*已排队，待运行*/
		log_debug("work %p will been canceling with queueing from %p [%p[%s]]",
			work, pwq, pwq->wq, pwq->wq->name);

		if (READ_ONCE(work->flags) & WQ_WORK_DELAYED) {
			/*必须激活调度的任务，否则将影响执行并发度，并会破坏排队结构*/
			/*有可能搬移了跟在 work 后的 linked 直接调度任务
			 *可能没有唤醒任何工作线程来处理 linked 任务？*/
			pwq_activate_delayed_work(pwq, work);
		}
		list_del_init(&work->entry);
		WQ_WARN(work->flags & WQ_WORK_BARRIER);
		/*必须释放 work 与 pwq 关联的引用计数，并激活后续的延迟任务*/
		pwq_dec_nr_in_flight(pwq, work->flags & WQ_WORK_BARRIER);

		/*保证 其他并发的 cancel_work() 不执行该路径，而是一定等待再尝试*/
		set_work_pool_and_keep_pending(work, pool->id);

		/*从排队状态下回收*/
		grabbed = true;
	}
	put_pwq(pwq);
	put_work_pool_locked(pool);
	/*即将被运行或正在运行，或工作线程发生迁移？*/
	if (grabbed)
		return 1;
fail:
	if (work_is_canceling(work)) {
		/*被其他路径抢占了 pending，等待*/
		log_debug("cancel work was processing: %p", work);
		return -ENOENT;
	}
	sched_yield();
	/*处于即将运行，或正在运行，再次尝试 抢占 pending 标志*/
	log_debug("cancel work need try again : %p", work);
	return -EAGAIN;
}

bool __cancel_work_sync(struct work_struct *work, bool is_dwork)
{
	int rc;
	static DEFINE_WAIT_QUEUE_HEAD(cancel_waitq);

	work_start_cancel(work);

	log_debug("prepare cancel work : %p", work);

	do {
		rc = try_to_grab_pending(work, is_dwork);
		if (skp_unlikely(rc == -ENOENT)) {
			struct cwt_wait cwait;

			init_waitqueue_entry(&cwait.wait);
			cwait.wait.func = cwt_wakefn;
			cwait.work = work;
			prepare_to_wait_exclusive(&cancel_waitq, &cwait.wait);
			/*条件必须在加入队里后再次判断*/
			if (work_is_canceling(work)) {
				/*防止同时操作 cancel 状态 和 插入多个 barrier work */
				wait_on(&cwait.wait);
				log_debug("cancel work maybe finish on other path : %p", work);
			}
			finish_wait(&cancel_waitq, &cwait.wait);
		}
	} while (skp_unlikely(rc < 0));

	mark_work_canceling(work);

	/*需要等待运行完成*/
	if (skp_likely(wq_online))
		flush_work(work);

	clear_work_data(work);

	smp_rmb();
	if (waitqueue_active(&cancel_waitq))
		__wake_up_one(&cancel_waitq, work);

	work_finish_cancel(work);

	log_debug("finish cancel work : %p [%s]", work, !!rc ? "hit" : "miss");

	return !!rc;
}

bool __cancel_work(struct work_struct *work, bool is_dwork)
{
	int rc;

	work_start_cancel(work);

	log_debug("try cancel work : %p", work);

	do {
		rc = try_to_grab_pending(work, is_dwork);
	} while (skp_unlikely(rc == -EAGAIN));

	if (skp_unlikely(rc < 0)) {
		work_finish_cancel(work);
		/*正在取消将被忽略*/
		return false;
	}

	/*成功抢占了 pending*/
	set_work_pool_and_clear_pending(work, get_work_pool_id(work));

	log_debug("finish cancel work : %p", work);

	work_finish_cancel(work);

	return !!rc;
}

static void wq_barrier_func(struct work_struct *ptr)
{
	struct wq_barrier *barr = container_of(ptr, struct wq_barrier, work);
	log_debug("%p work's barrier %p has finish job", barr->whose, barr);
	complete(&barr->done);
}

static void insert_wq_barrier(struct pool_workqueue *pwq, struct wq_barrier *barr,
		struct work_struct *target, struct wq_worker *worker)
{
	struct list_head *head;
	unsigned long linked = 0;

	barr->whose = target;
	init_completion(&barr->done);
	INIT_WORK(&barr->work, wq_barrier_func);
	__set_bit(WQ_WORK_PENDING_BIT, &barr->work.flags);

	/*多个 flush 将被倒序执行*/
	if (worker) {
		/*待冲洗的任务已经在运行，则直接插入 scheduled 链表中*/
		head = worker->scheduled.next;
	} else {
		/*正在排队，则插入到 work 后面，标记为 linked ，这样便于批量处理*/
		head = target->entry.next;
		/*@see move_linked_works() 为了使多个 barrier work 被批处理
		 * 1. target 还不是 linked :
		 *    work[linked]--> barrier[unlinked]--> other work
		 *   当执行任务时，会通过 move_linked_works() 将 work 和 barrier
		 *   直接移动到 worker->scheduled 链表上进行处理
		 * 2. target 已经是 linked，即 后面已经跟了一个 barrier work :
		 *    work[linked]--> new barrier[linked]--> old barrier[unlinked]--> other work
		 *   当执行任务是，会通过 move_linked_works() 将 work 和 两个后续的 barrier
		 *   直接移动到 worker->scheduled 链表上进行处理
		 * 3. move_linked_works() 的语义就是除 linked 的 work 全部移动，其至多移动一个 unlinked 的 work
		 */
		linked = target->flags & WQ_WORK_LINKED;
		__set_bit(WQ_WORK_LINKED_BIT, &target->flags);
	}

	insert_work(pwq, &barr->work, head, linked | WQ_WORK_BARRIER);
}

static bool start_flush_work(struct work_struct *work, struct wq_barrier *barr)
{
	bool rc = false;
	struct worker_pool *pool;
	struct pool_workqueue *pwq;
	struct wq_worker *worker = NULL;

	pool = get_work_pool_and_lock(work);
	/*没有关联的 pool 即 没有排队或运行*/
	if (!pool)
		return false;

	pwq = get_pwq(get_work_pwq(work));
	if (pwq) {
		/*任务在排队，但是任务发生了漂移？*/
		if (WARN_ON(pwq->pool != pool))
			goto already_gone;
	} else {
		/*任务在运行*/
		worker = find_worker_executing_work(pool, work);
		if (!worker)
			/*运行完成*/
			goto already_gone;
		/*递归调用，死锁*/
		BUG_ON(worker == current_wq_worker());
		pwq = get_pwq(worker->curr_pwq);
	}
	/*插入一个栅栏，在目标任务完成调度后一定会被运行*/
	insert_wq_barrier(pwq, barr, work, worker);
	rc = true;

already_gone:

	put_pwq(pwq);
	put_work_pool_locked(pool);

	return rc;
}

bool flush_work(struct work_struct *work)
{
	struct wq_barrier barr;

	if (WARN_ON(!wq_online))
		return false;

	if (start_flush_work(work, &barr)) {
		wait_for_completion(&barr.done);
		return true;
	}
	return false;
}

void delayed_work_timer_cb(struct uev_timer *ptr)
{
	struct delayed_work *dwork = container_of(ptr, struct delayed_work, timer);
	__queue_work(dwork->cpu, dwork->wq, &dwork->work);
}

static bool wq_queue_delayed_work(int cpu, struct workqueue_struct *wq,
		struct delayed_work *dwork, uint32_t delay)
{
	struct uev_timer *timer = &dwork->timer;
	struct work_struct *work = &dwork->work;

	if (WQ_WARN_ON(!wq) ||
		WQ_WARN_ON(timer->func != delayed_work_timer_cb) ||
		WQ_WARN_ON(uev_timer_pending(timer)) ||
		WQ_WARN_ON(!list_empty_careful(&work->entry)))
		return false;

	if (skp_unlikely(!delay))
		return __queue_work(cpu, wq, work);

	dwork->wq = wq;
	dwork->cpu = cpu;

	return !WARN_ON(__uev_timer_modify(timer, delay));
}

bool queue_delayed_work_on(int cpu, struct workqueue_struct *wq,
	struct delayed_work *dwork, uint32_t delay)
{
	if (WARN_ON(!wq))
		return false;
	if (!test_and_set_bit(WQ_WORK_PENDING_BIT, &dwork->work.flags))
		return wq_queue_delayed_work(cpu, wq, dwork, delay);

	return false;
}

bool mod_delayed_work_on(int cpu, struct workqueue_struct *wq,
	struct delayed_work *dwork, uint32_t delay)
{
	int rc;

	if (WARN_ON(!wq))
		return false;

	do {
		rc = try_to_grab_pending(&dwork->work, true);
	} while (skp_unlikely(rc == -EAGAIN));

	/*成功抢占了 pending 标志*/
	if (rc >= 0)
		return wq_queue_delayed_work(cpu, wq, dwork, delay);
	/*rc 为 -ENOENT 则其他路径正在同步取消*/
	return rc;
}

bool flush_delayed_work(struct delayed_work *dwork)
{
	if (uev_timer_delete_sync(&dwork->timer) &&
			!__queue_work(dwork->cpu, dwork->wq, &dwork->work))
		return false;
	return flush_work(&dwork->work);
}

uint32_t delayed_work_busy(struct delayed_work *dwork)
{
	if (uev_timer_pending(&dwork->timer) ||
			uev_ev_timer(&dwork->timer) == &dwork->timer)
		return WORK_BUSY_PENDING;
	return work_busy(&dwork->work);
}

struct work_struct * current_work(void)
{
	struct wq_worker *worker = current_wq_worker();
	return worker ? worker->curr_work : NULL;
}

int schedule_on_each_cpu(work_fn func)
{
	int cpu;
	DEFINE_PER_CPU(struct work_struct, works);

	if (skp_unlikely(!func))
		return -EINVAL;

	for_each_possible_cpu(cpu) {
		INIT_WORK(&per_cpu(works, cpu), func);
		schedule_work_on(cpu, &per_cpu(works, cpu));
	}

	for_each_possible_cpu(cpu) {
		flush_work(&per_cpu(works, cpu));
	}

	return 0;
}

static void rcu_work_rcufn(struct rcu_head *rcu)
{
	struct rcu_work *rwork = container_of(rcu, struct rcu_work, rcu);

	/* read the comment in __queue_work() */
	__queue_work(WQ_WORK_CPU_UNBOUND, rwork->wq, &rwork->work);
}

bool queue_rcu_work(struct workqueue_struct *wq, struct rcu_work *rwork)
{
	struct work_struct *work = &rwork->work;

	if (!test_and_set_bit(WQ_WORK_PENDING_BIT, &work->flags)) {
		rwork->wq = wq;
		call_rcu_sched(&rwork->rcu, rcu_work_rcufn);
		return true;
	}

	return false;
}

bool flush_rcu_work(struct rcu_work *rwork)
{
	if (work_pending(&rwork->work)) {
		rcu_barrier();
		WARN_ON(!flush_work(&rwork->work));
		return true;
	}
	return flush_work(&rwork->work);
}

void __wq_worker_sleeping(struct wq_worker* worker)
{
	struct worker_pool *pool;
	if (skp_unlikely(in_atomic()) ||
			READ_ONCE(worker->flags) & WORKER_NOT_RUNNING)
		return;

	pool = worker->pool;
	if (atomic_dec_and_test(&pool->nr_running) &&
			!list_empty_careful(&pool->worklist)) {
		lock_pool(pool);
		if (!list_empty(&pool->worklist))
			wake_up_worker(pool);
		unlock_pool(pool);
	}
}

/*工作线程启动了，从 I/O 或 休眠中返回调用*/
void __wq_worker_waking_up(struct wq_worker* worker)
{
	if (skp_unlikely(in_atomic()) ||
		READ_ONCE(worker->flags) & WORKER_NOT_RUNNING)
		return;
	atomic_inc(&worker->pool->nr_running);
}

#ifdef WQ_STAT
static void work_calc_stat(struct work_stat *stat, const struct work_struct *work)
{
	uint64_t sched_cost1 = 0, sched_cost2 = 0, process_cost = 0, cancel_cost = 0;
	if (work->time_point[wq_work_start_sched] &&
			work->time_point[wq_work_finish_sched])
		sched_cost1 = work->time_point[wq_work_finish_sched] -
			work->time_point[wq_work_start_sched];
	if (work->time_point[wq_work_finish_sched] &&
			work->time_point[wq_work_start_process])
		sched_cost2 = work->time_point[wq_work_start_process] -
			work->time_point[wq_work_finish_sched];
	if (work->time_point[wq_work_start_process] &&
			work->time_point[wq_work_finish_process])
		process_cost = work->time_point[wq_work_finish_process] -
			work->time_point[wq_work_start_process];
	if (work->time_point[wq_work_start_cancel] &&
			work->time_point[wq_work_finish_cancel])
		cancel_cost = work->time_point[wq_work_finish_cancel] -
			work->time_point[wq_work_start_cancel];

	stat->dispatch_cost = cycles_to_ns(sched_cost1);
	stat->sched_cost =  cycles_to_ns(sched_cost2);
	stat->process_cost = cycles_to_ns(process_cost);
	stat->cancel_cost = cycles_to_ns(cancel_cost);
}

void work_acc_stat(struct work_stat *dst, const struct work_struct *work)
{
	struct work_stat tmp;

	work_calc_stat(&tmp, work);

	dst->dispatch_cost += tmp.dispatch_cost;
	dst->sched_cost += tmp.sched_cost;
	dst->process_cost += tmp.process_cost;
	dst->cancel_cost += tmp.cancel_cost;
}
#endif
