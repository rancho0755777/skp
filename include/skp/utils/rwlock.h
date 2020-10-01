#ifndef __US_RWLOCK_H__
#define __US_RWLOCK_H__

#include "spinlock.h"


__BEGIN_DECLS

typedef struct rwlock {
	union {
		atomic_t cnts;
		struct {
#ifdef __LITTLE_ENDIAN
			uint8_t wlocked;	/* Locked for write? */
			uint8_t __lstate[3];
#else
			uint8_t __lstate[3];
			uint8_t wlocked;	/* Locked for write? */
#endif
		};
	};
	spinlock_t		wait_lock;
#ifdef RWLOCK_DEBUG
	uint32_t magic;
	int32_t owner_thread;
#endif
} rwlock_t;

#define __RW_LOCK_UNLOCKED ATOMIC_INIT(0)
#ifdef RWLOCK_DEBUG
#define	__RW_LOCK_INITIALIZER(lockname) \
	{ .cnts = __RW_LOCK_UNLOCKED, .wait_lock = __SPIN_LOCK_INITIALIZER(lockname),\
		.magic = MAGIC_U32, .owner_thread = __LOCK_OWNER_INIT, }
# define debug_rwlock_init(lock) \
	({ lock->magic = MAGIC_U32; lock->owner_thread = __LOCK_OWNER_INIT; })
# define debug_read_lock_before(lock) \
	({ __LOCK_BUG_ON((lock), (lock)->magic != MAGIC_U32, before); })
# define debug_read_unlock(lock) \
	({ __LOCK_BUG_ON((lock), (lock)->magic != MAGIC_U32, unlock); })
# define debug_write_lock_before(lock) \
	({ __LOCK_BUG_ON((lock), (lock)->magic != MAGIC_U32, before); \
		__LOCK_BUG_ON((lock), (lock)->owner_thread == get_thread_id(), before); })
# define debug_write_lock_after(lock) \
	({ (lock)->owner_thread = get_thread_id(); })
# define debug_write_unlock(lock) \
	({ __LOCK_BUG_ON((lock), (lock)->magic != MAGIC_U32, unlock); \
		__LOCK_BUG_ON((lock), (lock)->owner_thread != get_thread_id(), unlock); \
		static_mb(); \
		(lock)->owner_thread = __LOCK_OWNER_INIT; })
#else
#define	__RW_LOCK_INITIALIZER(lockname) \
	{ .cnts = __RW_LOCK_UNLOCKED, .wait_lock = __SPIN_LOCK_INITIALIZER(lockname), }
# define debug_rwlock_init(lock)
# define debug_read_lock_before(lock)
# define debug_read_unlock(lock)
# define debug_write_lock_before(lock)
# define debug_write_lock_after(lock)
# define debug_write_unlock(lock)
#endif

#define DEFINE_RWLOCK(x) rwlock_t x = __RW_LOCK_INITIALIZER(x)

static inline void rwlock_init(rwlock_t *lock)
{
	debug_rwlock_init(lock);
	spin_lock_init(&lock->wait_lock);
	lock->cnts = (atomic_t) __RW_LOCK_UNLOCKED;
}

/*
 * Writer states & reader shift and bias.
 * |<----8---->|<----1---->|<----others---->|
 * writer hold  writer wait  reader hold
 */
#define	_QW_WAITING	0x0100		/* A writer is waiting	   */
#define	_QW_LOCKED	0x00ff		/* A writer holds the lock */
#define	_QW_WMASK	0x01ff		/* Writer mask		   */
#define	_QR_SHIFT	9		/* Reader count shift	   */
#define _QR_BIAS	(1 << _QR_SHIFT)

/*
 * External function declarations
 */
extern void queued_read_lock_slowpath(rwlock_t *lock);
extern void queued_write_lock_slowpath(rwlock_t *lock);

static __always_inline bool queued_read_trylock(rwlock_t *lock)
{
	int32_t cnts;
	cnts = atomic_read(&lock->cnts);
	if (skp_likely(!(cnts & _QW_WMASK))) {
		cnts = atomic_add_return(_QR_BIAS, &lock->cnts);
		if (skp_likely(!(cnts & _QW_WMASK))) {
			return true;
		}
		atomic_sub(_QR_BIAS, &lock->cnts);
	}
	return false;
}

static __always_inline bool queued_write_trylock(rwlock_t *lock)
{
	int32_t cnts;
	cnts = atomic_read(&lock->cnts);
	if (skp_unlikely(cnts))
		return false;
	return skp_likely(atomic_cmpxchg(
		&lock->cnts, cnts, cnts | _QW_LOCKED) == cnts);
}

static __always_inline void queued_read_lock(rwlock_t *lock)
{
	int32_t cnts;
	cnts = atomic_add_return(_QR_BIAS, &lock->cnts);
	if (skp_likely(!(cnts & _QW_WMASK)))
		return;
	/* The slowpath will decrement the reader count, if necessary. */
	queued_read_lock_slowpath(lock);
}

static __always_inline void queued_write_lock(rwlock_t *lock)
{
	/* Optimize for the unfair lock case where the fair flag is 0. */
	if (atomic_cmpxchg(&lock->cnts, 0, _QW_LOCKED) == 0)
		return;
	queued_write_lock_slowpath(lock);
}

static __always_inline void queued_read_unlock(rwlock_t *lock)
{
	/*
	 * Atomically decrement the reader count
	 */
	atomic_sub_return(_QR_BIAS, &lock->cnts);
}

static inline void queued_write_unlock(rwlock_t *lock)
{
	/*
	 * only release lower 8 bits that indicated writer was locked
	 */
	__store_release(&lock->wlocked, 0);
}

////////////////////////////////////////////////////////////////////////////////
static inline void read_lock(rwlock_t *lock)
{
	enter_atomic();
	debug_read_lock_before(lock);
	queued_read_lock(lock);
}

static inline bool read_trylock(rwlock_t *lock)
{
	enter_atomic();
	if (queued_read_trylock(lock))
		return true;
	leave_atomic();
	return false;
}

static inline void read_unlock(rwlock_t *lock)
{
	debug_read_unlock(lock);
	queued_read_unlock(lock);
	leave_atomic();
}

////////////////////////////////////////////////////////////////////////////////
static inline void write_lock(rwlock_t *lock)
{
	enter_atomic();
	debug_write_lock_before(lock);
	queued_write_lock(lock);
	debug_write_lock_after(lock);
}

static inline bool write_trylock(rwlock_t *lock)
{
	enter_atomic();
	if (queued_write_trylock(lock)) {
		debug_write_lock_after(lock);
		return true;
	}
	leave_atomic();
	return false;
}

static inline void write_unlock(rwlock_t *lock)
{
	debug_write_unlock(lock);
	queued_write_unlock(lock);
	leave_atomic();
}
////////////////////////////////////////////////////////////////////////////////

__END_DECLS

#endif
