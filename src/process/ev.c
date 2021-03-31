#define _GNU_SOURCE
#include <fcntl.h>
#include <sys/time.h>
#include <sys/types.h>

#include <skp/process/ev.h>
#include <skp/process/signal.h>
#include <skp/mm/slab.h>

#define __REACTOR_IMPL_MAGIC__ 0xdeadbeef
/*信号的ID*/
#define EV_SIGID U32_MAX
/*事件线程默认检查时间，毫秒*/
#define EV_POLL_INTERVAL 5000u
/*单个循环能管理事件最大数目*/
#define PER_LOOPER_EVENTS_MAX (U16_MAX - 1)

/*配置单个线程最大能管理事件的数量*/
#ifndef CONFIG_PER_LOOPER_EVENTS_MAX
# define CONFIG_PER_LOOPER_EVENTS_MAX PER_LOOPER_EVENTS_MAX
#endif

/*每次从内核中抽取就绪事件的数量*/
#ifndef CONFIG_PER_POLL_EVENTS
# ifdef EVENT_DEBUG
#  define CONFIG_PER_POLL_EVENTS 32
# else
#  define CONFIG_PER_POLL_EVENTS 1024
# endif
#endif

#define EVENT_DEBUG

#ifdef EVENT_DEBUG
# define EVENT_BUG_ON(x) BUG_ON(x)
# define EVENT_WARN_ON(x) WARN_ON(x)
# define check_remain_stream(looper) do { 								\
for (int i = 0; looper->nr_events; i++) { 								\
	struct uev_event *event = idr_remove(&looper->evidr, i); 			\
	if (skp_unlikely(event)) 											\
		log_warn("stream %p was still left in event looper", event); 	\
}} while(0)
# define check_remain_signal(looper) do {								\
for (int i = 0; looper->siginfo && i < NSIG; i++) { 					\
	if (skp_unlikely(test_bit(i, looper->siginfo->registered))) 		\
		log_warn("signal %d was still in registering", i); 				\
}} while (0)
#else
# define EVENT_BUG_ON(x) ((void)(x))
# define EVENT_WARN_ON(x) ((void)(x))
# define check_remain_stream(looper)
# define check_remain_signal(looper)
#endif

static LIST__HEAD(looper_list);

#define uev_core_type(core) (READ_ONCE((core)->flags) & EVENT_TYPE_MASK)
#define uev_is_stream(core) (uev_core_type(core) == EVENT_STREAM)
#define uev_is_timer(core) (uev_core_type(core) == EVENT_TIMER)
#define uev_is_signal(core) (uev_core_type(core) == EVENT_SIGNAL)
#define uev_is_async(core) (uev_core_type(core) == EVENT_ASYNC)

#define looper_lock_init(l) mutex_init(&(l)->lock)
#define looper_need_lock(l) ((l)->flags & EVLOOP_PRIVATE)
#define looper_lock(l)						\
	do { if (looper_need_lock((l))) mutex_lock(&(l)->lock); } while (0)
#define looper_trylock(l)					\
	do { if (looper_need_lock((l))) mutex_trylock(&(l)->lock); } while (0)
#define looper_unlock(l)					\
	do { if (looper_need_lock((l))) mutex_unlock(&(l)->lock); } while (0)
#define looper_maybe_contented(l)			\
	do { if (looper_need_lock((l))) cond_resched_mutex(&(l)->lock); } while (0)

struct poll_event {
	uint16_t mask;
	/*low 32bits is soft id, high 32bits is hard fd*/
	uint64_t data;
};

#define pollevent_id(e) ((uint32_t)((e)->data & ((1ULL << 32) - 1)))
#define pollevent_fd(e) ((uint32_t)((e)->data >> 32))
#define pollevent_make_data(id, fd)								\
	(((uint64_t)(uint32_t)(id)) | ((uint64_t)(uint32_t)(fd)) << 32)
#define pollevent_init(e, i, f, m)								\
	do {														\
		(e)->mask = (m);										\
		(e)->data = pollevent_make_data((i), (f));				\
	} while(0)

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
__def_core_flag_op(TIMEDOUT)

#undef __def_core_flag_op

/*declare implement function*/
#define __declare_reactor_ctl(name) 							\
static int 														\
name##_event(struct uev_looper*, struct uev_stream*, struct poll_event*)

__declare_reactor_ctl(register);
__declare_reactor_ctl(unregister);
__declare_reactor_ctl(modify);
__declare_reactor_ctl(enable);
__declare_reactor_ctl(disable);

#undef __declare_reactor_ctl

/*错误返回非零0*/
static int reactor_create(struct uev_looper*);
/*销毁不同实现的私有数据*/
static void reactor_destroy(struct uev_looper*);
/*返回就绪个数*/
static int reactor_poll(struct uev_looper*, int timeout);

/*注册/注销信号事件*/
static int register_signal(struct uev_looper *, int signo);
static int unregister_signal(struct uev_looper *, int signo);

/*将指定位置的内核事件转换为自定义的事件*/
static int pollevent_pull(struct uev_looper*, int, struct poll_event*);

/*include implement file must be here*/
#if defined(__linux__)
# include "./reactor/epoll.h"
#elif defined(__apple__)
# include "./reactor/kqueue.h"
#else
# error "not support uevent"
#endif

static __always_inline
int stream_insert(struct uev_looper *looper, struct uev_stream *ev,
		uint16_t mask)
{
	int rc;
	long id;
	struct poll_event pe;

	if (test_PENDING(&ev->core))
		return 0;

	EVENT_BUG_ON(test_set_PENDING(&ev->core));

	/*new insert*/
	id = idr_alloc(&looper->evidr, ev);
	if (WARN_ON(id < 0)) {
		rc = -ENOMEM;
		goto oom;
	}

	/*register event*/
	pollevent_init(&pe, id, ev->fd, mask);
	rc = register_event(looper, ev, &pe);
	if (skp_unlikely(rc)) {
		goto fail;
	}

	EVENT_BUG_ON(id > U16_MAX);

	looper->nr_events++;
	ev->id = (uint16_t)id;
	WRITE_ONCE(ev->mask, mask);

	log_debug("insert event : fd [%d], id [%d]", ev->fd, ev->id);
	return 1;

fail:
	idr_remove(&looper->evidr, id);
oom:
	clear_PENDING(&ev->core);
	return rc;
}

static int stream_remove(struct uev_looper *looper, struct uev_stream *ev)
{
	struct poll_event pe;
	struct uev_stream *tmp;

	if (!test_clear_PENDING(&ev->core))
		return 0;

	pollevent_init(&pe, ev->id, ev->fd, ev->mask);
	EVENT_WARN_ON(unregister_event(looper, ev, &pe));

	tmp = idr_remove(&looper->evidr, ev->id);
	EVENT_BUG_ON(tmp != ev);

	ev->id = U16_MAX;
	looper->nr_events--;

	log_debug("remove event : fd [%d], id [%d]", ev->fd, ev->id);
	return 1;
}

static __always_inline
struct uev_stream* stream_lookup(struct uev_looper *looper, int fd, uint32_t id)
{
	struct uev_stream *ev = idr_find(&looper->evidr, id);
	if (skp_unlikely(!ev || ev->fd != fd)) {
		log_warn_on(id != EV_SIGID,
			"can't find event : fd [%d/%d], id [%d]", fd, (ev?ev->fd:-1), id);
		return NULL;
	}
	/*ABA cause this*/
	EVENT_BUG_ON(!uev_stream_pending(ev));
	EVENT_BUG_ON(uev_stream_looper(ev) != looper);
	return ev;
}

static int insert_notifier(struct uev_looper *looper)
{
	struct poll_event pe;
	struct uev_async *async = &looper->notifier;
	struct uev_stream *stream = &async->stream;

	/*回调为空，仅仅为了中断 poller 使用*/
	uev_async_init(async, NULL);
	uev_async_setlooper(async, looper);

	BUG_ON(async->pipe_fd[0] != -1);
	BUG_ON(pipe2(async->pipe_fd, O_NONBLOCK|O_CLOEXEC));
	stream->mask = EVENT_READ;
	stream->fd = async->pipe_fd[0];
	BUG_ON(stream_insert(looper, stream));
	BUG_ON(test_set_ATTACHED(&stream->core));
	pollevent_init(&pe, stream->id, stream->fd, stream->mask);
	BUG_ON(register_event(looper, stream, &pe));
}

static void delete_notifier(struct uev_looper *looper)
{
	struct poll_event pe;
	struct uev_async *async = &looper->notifier;
	struct uev_stream *stream = &async->stream;
	pollevent_init(&pe, stream->id, stream->fd, stream->mask);
	BUG_ON(!stream_remove(looper, stream));
	BUG_ON(!test_clear_ATTACHED(&stream->core));
	WARN_ON(unregister_event(looper, stream, &pe));
	close(async->pipe_fd[0]);
	close(async->pipe_fd[1]);
}

static inline void notify_poller(struct uev_looper *looper)
{
	int rc;
	/*没有绑定线程*/
	if (!looper->worker_thread)
		return;
	/*仅不在同线程时才通知*/
	if (looper->worker_thread == current)
		return;
	rc = uev_async_emit(&looper->notifier);
	EVENT_BUG_ON(rc);
}

static inline void notify_poller_locked(struct uev_looper *looper, bool updated)
{
	/*TODO:判断是否在处理事件，以减少一次IO*/
	looper_unlock(looper);
	/*防止阻塞先解锁 */
	if (updated)
		notify_poller(looper);
}

static struct uev_looper *
get_looper_and_lock(struct uev_core *core)
{
	struct uev_looper *looper;
	do {
		looper = uev_core_looper(core);
		if (skp_unlikely(!looper)) {
			/*TODO:查找可用的并设置*/
			return NULL;
		}
		looper_lock(looper);
		if (skp_likely(uev_core_looper(core) == looper))
			break;
		looper_unlock(looper);	
	} while (1);
	return looper;
}

static
int uev_wait_finish_sync(struct uev_core *core, struct uev_looper *looper)
{
	DEFINE_WAITQUEUE(wait);
	struct uev_looper *tmp;

	if (!looper_need_lock(looper))
		return 0;

	if (looper->current != core)
		return 0;

	/*TODO:事件线程当前的运行事件*/

	add_wait_queue_locked(&looper->wait_queue, &wait);

	do {
		if (looper->current != core)
			break;
		looper_unlock(looper);
		log_debug("wait event finish : %p/%p", core, uev_core_looper(core));
		/*以超时等待，因为 事件模块 可能已经被停止*/
		wait_on_timeout(&wait, 2000);
		looper_lock(looper);
	} while (test_bit(EVLOOP_RUNNING, &looper->flags));

	remove_wait_queue_locked(&looper->wait_queue, &wait);

	/*解锁了，有可能修改了 CPU 索引*/
	tmp = uev_core_looper(core);
	if (skp_unlikely(tmp != looper))
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
	struct uev_looper *looper;

	big_lock();
	list_for_each_entry(looper, &looper_list, node) {
		if (looper->current != core)
			continue;

		/*TODO:事件线程当前的运行事件*/

		looper_lock(looper);
		rc = uev_wait_finish_sync(core, looper);
		looper_unlock(looper);

		WARN_ON(rc == -EBUSY);
	}
	big_unlock();
}

void uev_stream_init(struct uev_stream *stream, int32_t fd, uev_stream_fn func)
{
	memset(stream, 0, sizeof(*stream));
	stream->fd = fd;
	stream->func = func;
	stream->id = U16_MAX;
	stream->core.flags = EVENT_STREAM|EVENT_WRITE_ONCE;
}

int __uev_stream_modify(struct uev_stream *stream, uint16_t mask)
{
	int rc = 0;
	struct poll_event pe;
	struct uev_looper *looper;

	/*不允许使用不是输入的事件*/
	if (WARN_ON(mask & ~(EVENT_ACTION_MASK|EVENT_EDGE)))
		return -EINVAL;

	/*在锁外预分配*/
	radix_tree_preload();
	/*获取槽位，有可能因系统异步关闭而失败*/
	looper = get_looper_and_lock(&stream->core);
	if (skp_unlikely(!looper))
		return -ENODEV;

	/*插入到管理集合，分配了一个ID，并注册，如果已注册则继续下面的流程，修改*/
	rc = stream_insert(looper, stream, mask);
	if (skp_unlikely(rc))
		goto out;

	/*modify*/
	/*构造一个通用事件标识，用于与不同平台的内核通信*/
	pollevent_init(&pe, stream->id, stream->fd, mask);
	if (skp_likely(mask || stream->mask) && mask != stream->mask) {
		rc = modify_event(looper, stream, &pe);
		if (skp_unlikely(rc))
			goto fail;
	}

	pe.mask &= stream->mask;
	if ((stream->mask & EVENT_EDGE) &&
			skp_likely((pe.mask) & EVENT_ACTION_MASK)) {
		/*如果是边沿触发，调整侦听的新旧共有事件*/
		rc = enable_event(looper, stream, &pe);
		if (skp_unlikely(rc))
			goto fail;
	}

	WRITE_ONCE(stream->mask, mask);
	looper_unlock(looper);
	return 0;

fail:
	stream_remove(looper, stream);
out:
	looper_unlock(looper);
	return rc>0?0:rc;
}

int __uev_stream_delete(struct uev_stream *stream, bool sync,
		struct uev_looper *nlooper)
{
	int rc, rc2, index;
	struct uev_looper *looper;

try:
	/*如果索引为无效值，那么可能处于不确定的状态，检查所有的事件线程*/
	looper = uev_stream_looper(stream);
	if (skp_unlikely(!looper))
		goto slow;

	looper = get_looper_and_lock(&stream->core);
	if (skp_unlikely(!looper))
		goto slow;

pending:
	rc = stream_remove(looper, stream);
	if (sync) {
		/*同步等待*/
		rc2 = uev_wait_finish_sync(&stream->core, looper);
		if (skp_unlikely(rc2)) {
			if (rc2 == -EAGAIN) {
				looper_unlock(looper);
				goto try;
			}
			if (!test_bit(EVLOOP_RUNNING, &looper->flags))
				goto out;
			goto pending;
		}
		uev_stream_setlooper(stream, nlooper);
	}

out:
	looper_unlock(looper);
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
void async_stream_cb(struct uev_stream *stream, uint16_t mask)
{
	int nr;
	char buff[32];
	struct uev_async *async =
		container_of(stream, struct uev_async, stream);

	EVENT_BUG_ON(!uev_is_async(&stream->core));

	do {
		ssize_t b = read(async->pipe_fd[0], buff, ARRAY_SIZE(buff));
		if (skp_likely(b < ARRAY_SIZE(buff))) {
			EVENT_WARN_ON(!b);
			EVENT_WARN_ON(b < 0 && errno != EAGAIN);
			break;
		}
	} while (1);

	nr = atomic_xchg(&async->nr_cnt, -1);
	EVENT_WARN_ON(nr < 0);

	if (async->func)
		async->func(async);

#ifdef EVENT_DEBUG
	if (&uev_async_looper(async)->notifier == async)
		log_debug("eat notifier : %d", nr + 1);
#endif
}

void uev_async_init(struct uev_async *async, uev_async_fn func)
{
	memset(async, 0, sizeof(*async));
	async->func = func;
	atomic_set(&async->nr_cnt, -1);
	async->pipe_fd[0] = async->pipe_fd[1] = -1;
	uev_stream_init(&async->stream, -1, async_stream_cb);
	/*修正类型*/
	async->stream.core.flags = EVENT_ASYNC|EVENT_WRITE_ONCE;
}

/*触发通知*/
int __uev_async_emit(struct uev_async *async)
{
	if (skp_unlikely(async->pipe_fd[1] < 0))
		return -EBADF;
	
	if (atomic_inc_and_test(&async->nr_cnt))
		if (write(async->pipe_fd[1], "1", 1) < 0) {
			if (errno != EAGAIN)
				return -errno;
		}

	return 0;
}
/**
 * 初始化 looper
 * @param looper 必须是 1U << EVENT_FLAGS_SHIFT 对齐的内存
 * @param poll_size 每次从内核中 拉取多少个就绪的事件，小于零则使用默认大小
 * @param flags 可以是无锁或不处理信号时间
 *
 * 不分配内存的路径都不检查返回值
 */
int uev_looper_init(struct uev_looper *looper, int poll_size, int flags)
{
	int rc;

	BUILD_BUG_ON(CONFIG_PER_POLL_EVENTS < 1);
	BUILD_BUG_ON(CONFIG_PER_LOOPER_EVENTS_MAX < 1);
	BUILD_BUG_ON(CONFIG_PER_LOOPER_EVENTS_MAX > PER_LOOPER_EVENTS_MAX);
	BUILD_BUG_ON(CONFIG_PER_POLL_EVENTS > CONFIG_PER_LOOPER_EVENTS_MAX);

	BUG_ON((uintptr_t)looper & EVENT_FLAGS_MASK);
	BUG_ON(flags & ~(EVLOOP_PRIVATE|EVLOOP_SIGNAL));

	poll_size = poll_size < 1 ? CONFIG_PER_POLL_EVENTS : poll_size;
	poll_size = poll_size > CONFIG_PER_LOOPER_EVENTS_MAX ? 
					CONFIG_PER_LOOPER_EVENTS_MAX : poll_size;

	looper->flags = flags;
	looper->nr_events = 0;
    looper->nr_triggers = 0;
    looper->reactor = NULL;
	looper->current = NULL;
	looper->worker_thread = NULL;
	looper->poll_size = poll_size;
	looper_lock_init(looper);
	init_waitqueue_head(&looper->wait_queue);

	/*timer*/
	looper->nr_timer = 0;
	looper->nr_timedout = 0;
	INIT_LIST_HEAD(&looper->rcu_queue);
	miniheap_init(&looper->timer_heap);

	/*signal*/
	looper->siginfo = NULL;
	if (flags & EVLOOP_SIGNAL) {
		/*TODO:检查是否已有一个信号处理循环对象*/
		rc = -ENOMEM;
		looper->siginfo = malloc(sizeof(*looper->siginfo));
		if (skp_unlikely(!looper->siginfo))
			goto sinfo_oom;
		memset(looper->siginfo, 0, sizeof(*looper->siginfo));
	}

	/*stream*/
	rc = idr_init_base(&looper->evidr, 0, CONFIG_PER_LOOPER_EVENTS_MAX);
	if (rc < 0)
		goto idr_oom;
	
	/*initialize reactor must be here*/
	rc = reactor_create(looper);
	if (rc < 0)
		goto fail;

	/*install notifer*/
	insert_notifier(looper);

	return 0;

fail:
	idr_destroy(&looper->evidr);
idr_oom:
	if (looper->siginfo)
		free(looper->siginfo);
sinfo_oom:
	return rc;
}

void uev_looper_finit(struct uev_looper* looper)
{
    BUG_ON((uintptr_t)looper & EVENT_FLAGS_MASK);

	notify_poller(looper);

	/*invoke all of event*/
	looper_lock(looper);
	process_rcus(looper, 1);
	process_timers(looper, 1);
	looper_unlock(looper);

	delete_notifier(looper);

	/*检查流事件*/
	check_remain_stream(looper);
	/*检查信号*/
	check_remain_signal(looper);

	reactor_destroy(looper);

	if (looper->siginfo)
		free(looper->siginfo);
}

static inline
void invoke_start(struct uev_looper *looper, struct uev_core *core)
{
	WRITE_ONCE(looper->current, core);
	looper_unlock(looper);
}
static __always_inline
void invoke_finish(struct uev_looper *looper)
{
	looper_lock(looper);
	EVENT_BUG_ON(!looper->current);

	WRITE_ONCE(looper->current, NULL);
	if (skp_likely(!waitqueue_active(&looper->wait_queue)))
		return;

	/*可能多个路径项删除事件，并处于等待状态*/
	wake_up_all_locked(&looper->wait_queue);
	looper_unlock(looper);
	sched_yield();
	looper_lock(looper);
}

static __always_inline
void stream_disable_locked(struct uev_looper *looper, struct uev_stream *stream,
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

	pollevent_init(&pe, stream->id, stream->fd, mask);
	modify_event(looper, stream, &pe);
	WRITE_ONCE(stream->mask, mask);
}

static void process_streams(struct uev_looper *looper, int nr_ready)
{
	int j, rc;
	uev_stream_fn func;
	unsigned long flags;
	uint16_t rmask, omask;
	struct poll_event pe;
	struct uev_stream *ev;
	uint32_t nr_invokes = 0;

	if (skp_unlikely(!looper->nr_events) || !nr_ready)
		return;

	/*增加IO随机性*/
	j = prandom_int(0, nr_ready - 1);
	for (int i = 0; i < nr_ready; i++) {
		if (++j >= nr_ready)
			j = 0;

		rc = pollevent_pull(looper, j, &pe);
		if (skp_unlikely(rc))
			continue;

		/*内核返回的描述符与用户层的标识符形成唯一键值对，防止描述符被重用，而
		 *事件系统却不知道这个事实*/
		ev = stream_lookup(looper, pollevent_fd(&pe), pollevent_id(&pe));
		/*流事件可能被异步删除了*/
		if (skp_unlikely(!ev)) {
			looper_maybe_contented(looper);
			continue;
		}

		rmask = 0;
		nr_invokes++;

		omask = ev->mask;
		flags = ev->core.flags;
		if (skp_likely(flags & EVENT_PENDING)) {
#ifdef __apple__
			/*如果是边沿触发，从内核中删除本次已触发的侦听事件*/
			if (skp_unlikely(omask & EVENT_EDGE))
				rmask = pe.mask;
			else
#endif
			/*移除一次性写事件*/
			if (skp_likely(flags & EVENT_WRITE_ONCE) &&
				skp_likely(!(omask & EVENT_EDGE)) && (pe.mask & EVENT_WRITE))
				rmask = EVENT_WRITE;
			stream_disable_locked(looper, ev, rmask);
		}

		/*关注的事件可能被异步修改了*/
		if (skp_unlikely(!(pe.mask & omask)) &&
				!(pe.mask & (EVENT_ERROR | EVENT_EOF))) {
			looper_maybe_contented(looper);
			continue;
		}

		func = ev->func;
		invoke_start(looper, &ev->core);
		if (skp_likely(func))
			func(ev, pe.mask);
		invoke_finish(looper);
	}

	looper->nr_triggers += nr_invokes;

	return;
}

int uev_loop(struct uev_looper *looper, int flags)
{
	int nr_ready;
	uint64_t now;
	uint32_t next;
	sigset_t sigset;

	EVENT_BUG_ON(flags & ~(EVLOOP_NOWAIT|EVLOOP_ONCE));
	EVENT_BUG_ON(looper->worker_thread && looper->worker_thread != current);

	if (looper->siginfo)
		signal_block_all(&sigset);
	__set_bit (EVLOOP_RUNNING, &looper->flags);

	next = EV_POLL_INTERVAL;
	do {
		next = flags & EVLOOP_NOWAIT ? 0 : next;
		nr_ready = reactor_poll(looper, next);
		if (nr_ready < 0)
			break;

		now = uev_timer_future(0);
		/*process ready event*/
		looper_lock(looper);

		process_streams(looper, nr_ready);
		process_rcus(looper, now);
		process_timers(looper, now);
		/*仅0号CPU会处理信号*/
		if (looper->siginfo)
			process_signals(looper);
		next = calc_timeout(looper, now);

		looper_unlock(looper);
	} while (!(flags & EVLOOP_ONCE)&&test_bit(EVLOOP_RUNNING, &looper->flags));

	__clear_bit(EVLOOP_RUNNING, &looper->flags);
	if (looper->siginfo)
		signal_unblock_all(&sigset);

	return nr_ready;
}
