/*
 * @Author: kai.zhou
 * @Date: 2018-09-11 11:01:33
 */
#ifndef __SU_SPINLOCK_H__
#define __SU_SPINLOCK_H__

#include "utils.h"

__BEGIN_DECLS

#define	_Q_SET_MASK(type)	\
	(((1U << _Q_ ## type ## _BITS) - 1) << _Q_ ## type ## _OFFSET)

#define _Q_LOCKED_OFFSET	0
#define _Q_LOCKED_BITS		8
#define _Q_LOCKED_MASK		_Q_SET_MASK(LOCKED)
#define _Q_LOCKED_VAL		(1U << _Q_LOCKED_OFFSET)

#define __SPINLOCK_UNLOCKED_VAL (0)
#define __SPINLOCK_UNLOCKED ATOMIC_INIT(__SPINLOCK_UNLOCKED_VAL)

extern __thread unsigned long atomic_ctx;
#define enter_atomic() ({ atomic_ctx += 1; static_mb(); })
#define leave_atomic() ({ static_mb(); atomic_ctx -= 1; })
#define in_atomic() (READ_ONCE(atomic_ctx))
#define maybe_sleep() (WARN_ON(in_atomic()))

typedef struct spinlock {
	atomic_t raw_lock;
#ifdef SPINLOCK_DEBUG
	uint32_t magic;
	int32_t owner_thread;
#endif
} spinlock_t;

#define __LOCK_OWNER_INIT (0)
#define __LOCK_BUG_ON(l, c, where)								\
do {															\
	bool __cond = !!(c); 										\
	if (skp_unlikely(__cond)) { 								\
		log_error("lock " #where " status : "					\
		"magic %x, owner : %u(%u)",								\
			(l)->magic, (l)->owner_thread, get_thread_id());	\
		BUG(); 													\
	} 															\
} while(0)

#ifdef SPINLOCK_DEBUG
# define __SPIN_LOCK_INITIALIZER(lockname)						\
	{															\
		.raw_lock = __SPINLOCK_UNLOCKED, .magic = MAGIC_U32,	\
		.owner_thread = __LOCK_OWNER_INIT,						\
	}
# define debug_spin_lock_before(lock)							\
	do {														\
		__LOCK_BUG_ON((lock), (lock)->magic != MAGIC_U32, 		\
			before); 											\
		__LOCK_BUG_ON((lock), (lock)->owner_thread == 			\
			get_thread_id(), before);							\
	} while (0)
# define debug_spinlock_init(lock)								\
	do {														\
		lock->magic = MAGIC_U32;								\
		lock->owner_thread = __LOCK_OWNER_INIT;					\
	} while(0)
# define debug_spin_lock_after(lock)							\
	do { (lock)->owner_thread = get_thread_id(); } while(0)
# define debug_spin_unlock(lock)								\
	do {														\
		__LOCK_BUG_ON((lock), (lock)->magic != MAGIC_U32,		\
			unlock);											\
		__LOCK_BUG_ON((lock), (lock)->owner_thread !=			\
			get_thread_id(), unlock);							\
		static_mb();											\
		(lock)->owner_thread = __LOCK_OWNER_INIT;				\
	} while(0)
#else
# define __SPIN_LOCK_INITIALIZER(lockname)						\
	{ .raw_lock = __SPINLOCK_UNLOCKED, }
# define debug_spinlock_init(lock)
# define debug_spin_lock_before(lock)
# define debug_spin_lock_after(lock)
# define debug_spin_unlock(lock)
#endif

#define DEFINE_SPINLOCK(x) spinlock_t x = __SPIN_LOCK_INITIALIZER(x)

static inline void spin_lock_init(spinlock_t *lock)
{
	debug_spinlock_init(lock);
	lock->raw_lock = (atomic_t)__SPINLOCK_UNLOCKED;
}

////////////////////////////////////////////////////////////////////////////////
static inline bool queued_spin_is_locked(spinlock_t *lock)
{
	return !!atomic_read(&lock->raw_lock);
}

static inline bool queued_spin_value_unlocked(spinlock_t lock)
{
	return !atomic_read(&lock.raw_lock);
}

static inline bool queued_spin_is_contended(spinlock_t *lock)
{
	/*except lower 8 bit*/
	return !!(atomic_read(&lock->raw_lock) & ~ _Q_LOCKED_MASK);
}
////////////////////////////////////////////////////////////////////////////////
static __always_inline bool queued_spin_trylock(spinlock_t *lock)
{
	if (!atomic_read(&lock->raw_lock) &&
			(atomic_cmpxchg(&lock->raw_lock, 0, _Q_LOCKED_VAL) == 0))
		return true;
	return false;
}

extern void queued_spin_lock_slowpath(spinlock_t *lock, uint32_t val);

/*最大竞争者数量，必须大于CPU数量*/
#define __SPINLOCK_MAX_CONTENDS 	(1<<5)
extern void __queued_spin_lock_fast(spinlock_t *lock);

static __always_inline void queued_spin_lock_fast(spinlock_t *lock)
{
	uint32_t val;
	val = atomic_cmpxchg(&lock->raw_lock, 0, _Q_LOCKED_VAL);
	if (skp_likely(!val))
		return;
	__queued_spin_lock_fast(lock);
}

static __always_inline void queued_spin_lock_fair(spinlock_t *lock)
{
	uint32_t val;
	val = atomic_cmpxchg(&lock->raw_lock, 0, _Q_LOCKED_VAL);
	if (skp_likely(!val))
		return;
	queued_spin_lock_slowpath(lock, val);
}

static inline void queued_spin_unlock(spinlock_t *lock)
{
	/*only change lower 8 bit*/
	__store_release((uint8_t*)&lock->raw_lock, 0);
}
////////////////////////////////////////////////////////////////////////////////
static inline void spin_lock(spinlock_t *lock)
{
	enter_atomic();
	debug_spin_lock_before(lock);
	queued_spin_lock_fast(lock);
	debug_spin_lock_after(lock);
}

/*公平排队*/
static inline void spin_fairlock(spinlock_t *lock)
{
	enter_atomic();
	debug_spin_lock_before(lock);
	queued_spin_lock_fair(lock);
	debug_spin_lock_after(lock);
}

static inline bool spin_trylock(spinlock_t *lock)
{
	enter_atomic();
	if (queued_spin_trylock(lock)) {
		debug_spin_lock_after(lock);
		return true;
	}
	leave_atomic();
	return false;
}

static inline void spin_unlock(spinlock_t *lock)
{
	debug_spin_unlock(lock);
	queued_spin_unlock(lock);
	leave_atomic();
}

static inline bool spinlock_is_locked(spinlock_t *lock)
{
	return queued_spin_is_locked(lock);
}

static inline bool spinlock_is_contended(spinlock_t *lock)
{
	return queued_spin_is_contended(lock);
}

static inline bool cond_resched_lock(spinlock_t * lock)
{
	if (spinlock_is_contended(lock)) {
		spin_unlock(lock);
		sched_yield();
		spin_fairlock(lock);
		return true;
	}
	return false;
}
////////////////////////////////////////////////////////////////////////////////
/*
 * This is an implementation of the notion of "decrement a
 * reference count, and return locked if it decremented to zero".
 *
 * NOTE NOTE NOTE! This is not equivalent to
 *
 *	if (atomic_dec_and_test(&atomic)) {
 *		spin_lock(&lock);
 *		return 1;
 *	}
 *	return 0;
 *
 * because the spin-lock and the decrement must be
 * "atomic".
 */
static __always_inline bool atomic_dec_and_lock(atomic_t *a, spinlock_t *l)
{
	/*测试是否已经为1*/
	if (atomic_add_unless(a, -1, 1))
		return false;
	spin_lock(l);
	if (atomic_dec_and_test(a))
		/*已经递减到0，则加锁，并返回真*/
		return true;
	spin_unlock(l);
	return false;
}
////////////////////////////////////////////////////////////////////////////////
static __always_inline void bit_spin_lock(int bit, unsigned long *addr)
{
	while (skp_unlikely(test_and_set_bit_lock(bit, addr))) {
		__cond_load_acquire(addr, !(test_bit(bit, &VAL)));
	}
}

static inline bool bit_spin_trylock(int bit, unsigned long *addr)
{
	return skp_likely(!test_and_set_bit_lock(bit, addr));
}

static inline void bit_spin_unlock(int bit, unsigned long *addr)
{
#ifdef DEBUG
	BUG_ON(!test_bit(bit, addr));
#endif
	clear_bit_unlock(bit, addr);
}

static inline bool bit_spin_is_locked(int bit, unsigned long *addr)
{
	return !!test_bit(bit, addr);
}
////////////////////////////////////////////////////////////////////////////////

__END_DECLS

#endif
