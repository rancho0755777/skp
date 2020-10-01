#include <skp/utils/mutex.h>

struct mutex_waiter {
	struct list_head	list;
	int flags;
};

#define count2mutex(c) container_of((c), struct mutex, count)

static void __mutex_lock_slowpath(atomic_t *);
static void __mutex_unlock_slowpath(atomic_t *);
static bool __mutex_trylock_slowpath(atomic_t *);

static inline void __mutex_fastpath_lock(atomic_t *count,
		void (*fail_fn)(atomic_t *))
{
	debug_mutex_prepare_lock(count2mutex(count));
	if (skp_unlikely(atomic_dec_return(count) < 0))
		/*有等待者，需要排队*/
		fail_fn(count);
	debug_mutex_set_owner(count2mutex(count));
}

static inline void __mutex_fastpath_unlock(atomic_t *count,
		void (*fail_fn)(atomic_t *))
{
	debug_mutex_clear_owner(count2mutex(count));
	if (skp_unlikely(atomic_inc_return(count) < 1))
		fail_fn(count);
	/*没有任何等待者*/
}

static inline bool __mutex_fastpath_trylock(atomic_t *count,
		bool (*fail_fn)(atomic_t *))
{
	debug_mutex_prepare_lock(count2mutex(count));
	if (skp_likely(atomic_cmpxchg(count, 1, 0) == 1)) {
		debug_mutex_set_owner(count2mutex(count));
		return true;
	}
	return false;
}

static void __mutex_lock_slowpath(atomic_t *lock_count)
{
	struct mutex_waiter waiter;
	struct mutex *lock = count2mutex(lock_count);

	waiter.flags = 0;
	spin_lock(&lock->wait_lock);
	/* add waiting tasks to the end of the waitqueue (FIFO): */
	list_add_tail(&waiter.list, &lock->wait_list);

	/*反复查看是否已经解锁，如果可能就抢占状态*/
	while (atomic_xchg(&lock->count, -1) != 1) {
		/*TODO:死锁、僵死锁检查？*/
		spin_unlock(&lock->wait_lock);
		futex_wait(&waiter.flags, 0, -1);
		spin_lock(&lock->wait_lock);
		waiter.flags = 0;
	}

	list_del(&waiter.list);

	/* set it to 0 if there are no waiters left: */
	if (skp_likely(list_empty(&lock->wait_list)))
		atomic_set(&lock->count, 0);

	spin_unlock(&lock->wait_lock);
}

static void __mutex_unlock_slowpath(atomic_t *lock_count)
{
	struct mutex_waiter *waiter;
	struct mutex *lock = count2mutex(lock_count);

	spin_lock(&lock->wait_lock);

	/*
	 * some architectures leave the lock unlocked in the fastpath failure
	 * case, others need to leave it locked. In the later case we have to
	 * unlock it here
	 */
	atomic_set(&lock->count, 1);

	/* get the first entry from the wait-list: */
	waiter = list_first_entry_or_null(
		&lock->wait_list, struct mutex_waiter, list);
	if (waiter) {
		xadd(&waiter->flags, 1);
		futex_wake(&waiter->flags, 1);
	}
	spin_unlock(&lock->wait_lock);
}

static bool __mutex_trylock_slowpath(atomic_t *lock_count)
{
	int prev;
	struct mutex *lock = count2mutex(lock_count);

	spin_lock(&lock->wait_lock);

	prev = atomic_xchg(&lock->count, -1);
	/* Set it back to 0 if there are no waiters: */
	if (skp_likely(list_empty(&lock->wait_list)))
		atomic_set(&lock->count, 0);

	spin_unlock(&lock->wait_lock);

	return prev == 1;
}

void mutex_lock(mutex_t *lock)
{
	__mutex_fastpath_lock(&lock->count, __mutex_lock_slowpath);
}

void mutex_unlock(mutex_t *lock)
{
	__mutex_fastpath_unlock(&lock->count, __mutex_unlock_slowpath);
}

bool mutex_trylock(mutex_t *lock)
{
	return __mutex_fastpath_trylock(&lock->count, __mutex_trylock_slowpath);
}

void recursive_mutex_lock(recursive_mutex_t *rmutex)
{
	int32_t thid = get_thread_id();
	if (READ_ONCE(rmutex->owner) == thid)
		goto out;
	mutex_lock(&rmutex->mutex);
	WRITE_ONCE(rmutex->owner, thid);
out:
	if (rmutex->nr_depths++)
		log_debug("recursive lock : %p, %u", rmutex, rmutex->nr_depths);
}

bool recursive_mutex_trylock(recursive_mutex_t *rmutex)
{
	int32_t thid = get_thread_id();
	if (READ_ONCE(rmutex->owner) == thid)
		goto out;
	if (!mutex_trylock(&rmutex->mutex))
		return false;
	WRITE_ONCE(rmutex->owner, thid);
out:
	if (rmutex->nr_depths++)
		log_debug("recursive lock : %p, %u", rmutex, rmutex->nr_depths);
	return true;
}

void recursive_mutex_unlock(recursive_mutex_t *rmutex)
{
#ifdef MUTEX_DEBUG
	int32_t thid = get_thread_id();
	BUG_ON(!mutex_is_locked(&rmutex->mutex));
	if (skp_unlikely(!rmutex->nr_depths || rmutex->owner != thid)) {
		log_warn("recursive unlock failed : "
			"%p tid %u, but %u", rmutex, rmutex->owner, thid);
		BUG();
	}
#endif
	if (--rmutex->nr_depths) {
#ifdef MUTEX_DEBUG
		BUG_ON(rmutex->nr_depths < 0);
#endif
		log_debug("recursive unlock : %p, %u", rmutex, rmutex->nr_depths);
		return;
	}
	WRITE_ONCE(rmutex->owner, 0);
	mutex_unlock(&rmutex->mutex);
}
