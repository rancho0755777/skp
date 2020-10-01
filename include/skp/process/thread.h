#ifndef SS_US_THREAD_H
#define SS_US_THREAD_H

#include <pthread.h>
#include "completion.h"

__BEGIN_DECLS

#ifndef CONFIG_THREAD_STACK
# define CONFIG_THREAD_STACK (128 * PAGE_SIZE)
#endif

struct _thread {
	unsigned long flags;
	pid_t tgid;
	pid_t pid;
	char *stack;
	completion_t started;
	completion_t stopped;
	pthread_t pthid;
	int ret;
};

typedef struct _thread *uthread_t;

enum {
/*这些位设置后不能清除*/
	THREAD_MAINTHREAD_BIT	= 0,
	THREAD_WAKING_BIT,
	THREAD_RUNNING_BIT,
	THREAD_STOPPING_BIT,
	THREAD_STOPPED_BIT,
	THREAD_DETACHED_BIT, /*< 已经分离*/
/*这些位用于区分预定义的子类线程*/
	THREAD_ISQUEUEWORKER_BIT,
	THREAD_ISEVENTWORKER_BIT,
};

#define THREAD_MAINTHREAD		(1UL << THREAD_MAINTHREAD_BIT)
#define THREAD_WAKING			(1UL << THREAD_WAKING_BIT)
#define THREAD_RUNNING			(1UL << THREAD_RUNNING_BIT)
#define THREAD_STOPPING			(1UL << THREAD_STOPPING_BIT)
#define THREAD_STOPPED			(1UL << THREAD_STOPPED_BIT)
#define THREAD_DETACHED			(1UL << THREAD_DETACHED_BIT)
#define THREAD_ISQUEUEWORKER	(1UL << THREAD_ISQUEUEWORKER_BIT)
#define THREAD_ISEVENTWORKER	(1UL << THREAD_ISEVENTWORKER_BIT)

typedef int (*thread_fn)(void*);

extern uthread_t __uthread_create(thread_fn fn, void *arg, size_t classize);

/*普通非继承线程*/
static inline uthread_t uthread_create(thread_fn fn, void *arg)
{
	return __uthread_create(fn, arg, sizeof(struct _thread));
}

extern int uthread_wakeup(uthread_t thread);

extern uthread_t uthread_run(thread_fn fn, void *arg);
extern int uthread_stop(uthread_t thread, int *exit_code);
/*尽量不使用此函数*/
extern int uthread_kill(uthread_t thread);
/*只能剥离自己*/
extern void uthread_detach(void);

extern __thread uthread_t current;

static inline bool uthread_mainthread(void)
{
	return skp_likely(current) && !!(current->flags & THREAD_MAINTHREAD);
}

static inline bool uthread_specialthread(void)
{
	return skp_likely(current) &&
		!!(current->flags & (THREAD_ISEVENTWORKER | THREAD_ISQUEUEWORKER));
}

static inline bool uthread_should_stop(void)
{
	return skp_likely(current) &&
		!!(current->flags & (THREAD_STOPPING | THREAD_STOPPED));
}

////////////////////////////////////////////////////////////////////////////////
// 继承类 线程
////////////////////////////////////////////////////////////////////////////////

struct wq_worker;
static inline bool uthread_is_wq_worker(uthread_t thread)
{
	return skp_likely(thread) && !!(thread->flags & THREAD_ISQUEUEWORKER);
}

static inline struct wq_worker *current_wq_worker(void)
{
	if (uthread_is_wq_worker(current))
		return (struct wq_worker*)current;
	return NULL;
}

////////////////////////////////////////////////////////////////////////////////
// 以下函数用于 I/O 和 休眠，如果是原子上下文 (in_atomic())，则不会有任何作用
////////////////////////////////////////////////////////////////////////////////
/*工作线程休眠了，I/O 或 休眠前调用，以便唤醒其他worker线程继续处理后续任务*/
extern void __wq_worker_sleeping(struct wq_worker*);
extern void __wq_worker_waking_up(struct wq_worker*);

/*工作线程休眠了，I/O 或 休眠前调用*/
static inline void wq_worker_sleeping(void)
{
	struct wq_worker *worker = current_wq_worker();
	if (worker) __wq_worker_sleeping(worker);
}

/*工作线程启动了，从 I/O 或 休眠中返回调用*/
static inline void wq_worker_waking_up(void)
{
	struct wq_worker *worker = current_wq_worker();
	if (worker) __wq_worker_waking_up(worker);
}

////////////////////////////////////////////////////////////////////////////////
struct uev_worker;

static inline bool uthread_is_ev_worker(uthread_t thread)
{
	return skp_likely(thread) && !!(thread->flags & THREAD_ISEVENTWORKER);
}

static inline struct uev_worker *current_ev_worker(void)
{
	if (uthread_is_ev_worker(current))
		return (struct uev_worker*)current;
	return NULL;
}

////////////////////////////////////////////////////////////////////////////////
// 注册的函数会在线程退出时，以先进后出的方式被回调，主要用于辅助清理TLS数据
// 清理函数必须使用绕过缓存的函数来释放数据，且不能使用任何 umalloc/pages 来
// 分配内存。如果主线程不使用 pthread_exit() 来退出，将不会触发。
// 而且要防止在回调中递归调用该函数。
////////////////////////////////////////////////////////////////////////////////

typedef void (*tls_cleaner)(void *);
extern void tlsclnr_register(tls_cleaner, void*);

__END_DECLS

#endif
