//
//  rwsem.c
//
//  Created by 周凯 on 2019/3/4.
//  Copyright © 2019 zhoukai. All rights reserved.
//

#include <skp/utils/rwsem.h>
#include <skp/utils/futex.h>

struct rwsem_waiter {
	struct list_head list;
	int flags;
#define RWSEM_WAITING_FOR_READ	0x10000000
#define RWSEM_WAITING_FOR_WRITE	0x20000000
#define RWSEM_WAITING_MASK		0x0fffffff
};

#define rwsem__init_lock(sem) spin_lock_init(&(sem)->wait_lock)
#define rwsem__lock(sem) spin_lock(&(sem)->wait_lock)
#define rwsem__unlock(sem) spin_unlock(&(sem)->wait_lock)

//#define RWSEM_DEBUG

#ifdef RWSEM_DEBUG
# define RWSEM_BUG_ON(x) BUG_ON(x)
static inline void rwsemtrace(struct rw_semaphore *sem, const char *str)
{
	log_debug("[%d] %s({%d,%d})",
		get_thread_id(), str, sem->activity,
		list_empty(&sem->wait_list) ? 0 : 1);
}
#else
# define RWSEM_BUG_ON(x)
# define rwsemtrace(sem, str)
#endif

static inline void __wake_one_waiter(struct rwsem_waiter *waiter)
{
	xadd32(&waiter->flags, 1);
	futex_wake(&waiter->flags, 1);
}

static inline void wake_one_waiter(struct rwsem_waiter *waiter)
{
	list_del(&waiter->list);
	smp_mb();
	__wake_one_waiter(waiter);
}

/*
 * initialise the semaphore
 */
void init_rwsem(struct rw_semaphore *sem)
{
	sem->activity = 0;
	rwsem__init_lock(sem);
	INIT_LIST_HEAD(&sem->wait_list);
}

/*
 * handle the lock release when processes blocked on it that can now run
 * - if we come here, then:
 *   - the 'active count' _reached_ zero
 *   - the 'waiting count' is non-zero
 * - the spinlock must be held by the caller
 * - woken process blocks are discarded from the list after having task zeroed
 * - writers are only woken if wakewrite is non-zero
 */
static inline struct rw_semaphore *
__rwsem_do_wake(struct rw_semaphore *sem, int wakewrite)
{
	struct rwsem_waiter *waiter;
	int woken;

	rwsemtrace(sem, "Entering __rwsem_do_wake");

	waiter = list_first_entry(&sem->wait_list, struct rwsem_waiter, list);

	if (waiter->flags & RWSEM_WAITING_FOR_WRITE) {
		if (wakewrite)
			__wake_one_waiter(waiter);
		goto out;
	}

	/* grant an infinite number of read locks to the front of the queue */
	woken = 0;
	do {
		struct list_head *next = waiter->list.next;
		wake_one_waiter(waiter);
		woken++;
		if (next == &sem->wait_list)
			break;
		waiter = list_entry(next, struct rwsem_waiter, list);
	} while (waiter->flags & RWSEM_WAITING_FOR_READ);

	sem->activity += woken;

out:
	rwsemtrace(sem, "Leaving __rwsem_do_wake");
	return sem;
}

/*
 * wake a single writer
 */
static inline struct rw_semaphore *
__rwsem_wake_one_writer(struct rw_semaphore *sem)
{
	struct rwsem_waiter *waiter;
	waiter = list_first_entry(&sem->wait_list, struct rwsem_waiter, list);
	BUG_ON(waiter->flags & RWSEM_WAITING_FOR_READ);
	__wake_one_waiter(waiter);
	return sem;
}

/*
 * get a read lock on the semaphore
 */
void down_read(struct rw_semaphore *sem)
{
	int flags;
	struct rwsem_waiter waiter;

	rwsemtrace(sem, "Entering __down_read");

	rwsem__lock(sem);

	/*读优先*/
	if (sem->activity > 0 || (sem->activity==0 &&
			list_empty(&sem->wait_list))) {
		/* granted */
		sem->activity++;
		goto out;
	}

	/* set up my own style of waitqueue */
	waiter.flags = RWSEM_WAITING_FOR_READ;
	list_add_tail(&waiter.list, &sem->wait_list);

	/* wait to be given the lock */
	for (;;) {
		flags = READ_ONCE(waiter.flags);
		if (flags & RWSEM_WAITING_MASK)
			break;
		rwsem__unlock(sem);
		futex_wait(&waiter.flags, flags, -1);
		rwsem__lock(sem);
	}
out:
	rwsem__unlock(sem);
	rwsemtrace(sem, "Leaving __down_read");
}

/*
 * trylock for reading -- returns 1 if successful, 0 if contention
 */
bool down_read_trylock(struct rw_semaphore *sem)
{
	bool ret = false;
	rwsemtrace(sem, "Entering __down_read_trylock");

	rwsem__lock(sem);

	if (sem->activity >= 0 && list_empty(&sem->wait_list)) {
		/* granted */
		sem->activity++;
		ret = true;
	}

	rwsem__unlock(sem);

	rwsemtrace(sem, "Leaving __down_read_trylock");
	return ret;
}

/*
 * get a write lock on the semaphore
 * - we increment the waiting count anyway to indicate an exclusive lock
 */
void down_write(struct rw_semaphore *sem)
{
	int flags;
	struct rwsem_waiter waiter;

	rwsemtrace(sem, "Entering __down_write");

	rwsem__lock(sem);

	waiter.flags = RWSEM_WAITING_FOR_WRITE;
	list_add_tail(&waiter.list, &sem->wait_list);

	/* wait to be given the lock */
	for (;;) {
		if (sem->activity == 0)
			break;
		flags = READ_ONCE(waiter.flags);
		rwsem__unlock(sem);
		futex_wait(&waiter.flags, flags, -1);
		rwsem__lock(sem);
		waiter.flags = RWSEM_WAITING_FOR_WRITE;
	}
	sem->activity = -1;
	list_del(&waiter.list);
	rwsem__unlock(sem);

	rwsemtrace(sem, "Leaving __down_write");
}

/*
 * trylock for writing -- returns 1 if successful, 0 if contention
 */
bool down_write_trylock(struct rw_semaphore *sem)
{
	bool ret = false;
	rwsemtrace(sem, "Entering __down_write_trylock");

	rwsem__lock(sem);
	if (sem->activity == 0) {
		/* granted */
		sem->activity = -1;
		ret = true;
	}
	rwsem__unlock(sem);

	rwsemtrace(sem, "Leaving __down_write_trylock");
	return ret;
}

/*
 * release a read lock on the semaphore
 */
void up_read(struct rw_semaphore *sem)
{
	rwsemtrace(sem, "Entering __up_read");

	rwsem__lock(sem);

	/*如果链表不为空，则一定有一个写路径在等待*/
	if (--sem->activity == 0 && !list_empty(&sem->wait_list))
		sem = __rwsem_wake_one_writer(sem);

	rwsem__unlock(sem);

	rwsemtrace(sem, "Leaving __up_read");
}

/*
 * release a write lock on the semaphore
 */
void up_write(struct rw_semaphore *sem)
{
	rwsemtrace(sem, "Entering __up_write");

	rwsem__lock(sem);

	sem->activity = 0;
	if (!list_empty(&sem->wait_list))
		sem = __rwsem_do_wake(sem, 1);

	rwsem__unlock(sem);

	rwsemtrace(sem, "Leaving __up_write");
}

void downgrade_write(struct rw_semaphore *sem)
{
	rwsemtrace(sem, "Entering __downgrade_write");

	rwsem__lock(sem);

	sem->activity = 1;
	if (!list_empty(&sem->wait_list))
		sem = __rwsem_do_wake(sem, 0);

	rwsem__unlock(sem);

	rwsemtrace(sem, "Leaving __downgrade_write");
}
