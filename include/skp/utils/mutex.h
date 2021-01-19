#ifndef __US_MUTEX_H__
#define __US_MUTEX_H__

#include "futex.h"
#include "spinlock.h"
#include "../adt/list.h"

__BEGIN_DECLS

typedef struct mutex {
	/** 1: unlocked, 0: locked, negative: locked, possible waiters */
	atomic_t		count;
	spinlock_t		wait_lock;
	struct list_head wait_list;
#ifdef MUTEX_DEBUG
	uint32_t magic;
	int32_t owner_thread;
#endif
} mutex_t;

#ifndef MUTEX_DEBUG
# define __MUTEX_INITIALIZER(lockname)						\
	{														\
		.count = ATOMIC_INIT(1),							\
		.wait_lock = __SPIN_LOCK_INITIALIZER(lockname),		\
		.wait_list = LIST_HEAD_INIT(lockname.wait_list),	\
	}
# define debug_mutex_init(x)
# define debug_mutex_prepare_lock(lock)
# define debug_mutex_set_owner(lock)
# define debug_mutex_clear_owner(lock)
#else
# define __MUTEX_MAGIC (0xdead0003U)
# define __MUTEX_OWNER_INIT (0)
# define __MUTEX_INITIALIZER(lockname)						\
	{														\
		.count = ATOMIC_INIT(1), 							\
		.wait_lock = __SPIN_LOCK_INITIALIZER(lockname),		\
		.wait_list = LIST_HEAD_INIT(lockname.wait_list), 	\
		.magic = __MUTEX_MAGIC,								\
		.owner_thread = __MUTEX_OWNER_INIT, 				\
	}
# define debug_mutex_init(x) 								\
	do {													\
		(x)->magic = __MUTEX_MAGIC;							\
		(x)->owner_thread = __MUTEX_OWNER_INIT;				\
	} while(0)
# define MUTEX_BUG_ON(l, c, where)							\
	do {													\
		bool __cond = !!(c); 								\
		if (skp_unlikely(__cond)) { 							\
			log_error("lock, %s status : " 					\
				"magic %x, owner : %u(%u)",					\
				(where), (l)->magic, (l)->owner_thread, 	\
				get_thread_id()); 							\
			BUG(); 											\
		} 													\
	} while(0)

static inline void debug_mutex_prepare_lock(struct mutex *lock)
{
	MUTEX_BUG_ON(lock, lock->owner_thread == get_thread_id(), "lock");
	MUTEX_BUG_ON(lock, lock->magic != __MUTEX_MAGIC, "lock");
	static_mb();
}
static inline void debug_mutex_set_owner(struct mutex *lock)
{
	MUTEX_BUG_ON(lock, lock->owner_thread != 0, "lock");
	MUTEX_BUG_ON(lock, lock->magic != __MUTEX_MAGIC, "lock");
	static_mb();
	lock->owner_thread = get_thread_id();
}
static inline void debug_mutex_clear_owner(struct mutex *lock)
{
	MUTEX_BUG_ON(lock, lock->owner_thread != get_thread_id(), "unlock");
	MUTEX_BUG_ON(lock, lock->magic != __MUTEX_MAGIC, "unlock");
	static_mb();
	lock->owner_thread = 0;
}
#endif

#define DEFINE_MUTEX(x)  mutex_t x = __MUTEX_INITIALIZER(x)

static inline void mutex_init(mutex_t *x)
{
	atomic_set(&x->count, 1);
	spin_lock_init(&x->wait_lock);
	INIT_LIST_HEAD(&x->wait_list);
	debug_mutex_init(x);
}

static inline bool mutex_is_locked(mutex_t *x)
{
	return atomic_read(&x->count) != 1;
}

extern void mutex_lock(mutex_t *mutex);
extern void mutex_unlock(mutex_t *mutex);
extern bool mutex_trylock(mutex_t *mutex);

static inline bool mutex_is_contended(mutex_t *mutex)
{
	return !list_empty(&mutex->wait_list);
}

static inline bool cond_resched_mutex(mutex_t * mutex)
{
	if (mutex_is_contended(mutex)) {
		mutex_unlock(mutex);
		sched_yield();
		mutex_lock(mutex);
		return true;
	}
	return false;
}

typedef struct recursive_mutex {
	mutex_t mutex;
	int32_t owner;
	int32_t nr_depths;
} recursive_mutex_t;

#define __RECURSIVE_MUTEX_INITIALIZER(lockname) \
	{ .mutex = __MUTEX_INITIALIZER(lockname.mutex), .owner=0, .nr_depths=0, }

#define DEFINE_RECURSIVE_MUTEX(name)			\
	recursive_mutex_t name = __RECURSIVE_MUTEX_INITIALIZER(name)

static inline void recursive_mutex_init(recursive_mutex_t *rmutex)
{
	rmutex->owner = 0;
	rmutex->nr_depths = 0;
	mutex_init(&rmutex->mutex);
}

extern void recursive_mutex_lock(recursive_mutex_t *rmutex);
extern bool recursive_mutex_trylock(recursive_mutex_t *rmutex);
extern void recursive_mutex_unlock(recursive_mutex_t *rmutex);

static __always_inline bool
	atomic_dec_and_mutex(atomic_t *a, mutex_t *l)
{
	/*测试是否已经为1*/
	if (atomic_add_unless(a, -1, 1))
		return false;
	mutex_lock(l);
	if (atomic_dec_and_test(a))
	/*已经递减到0，则加锁，并返回真*/
		return true;
	mutex_unlock(l);
	return false;
}

__END_DECLS

#endif
