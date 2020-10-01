#include <skp/utils/spinlock.h>

////////////////////////////////////////////////////////////////////////////////
/*
 * Bitfields in the atomic value:
 *
 * When NR_CPUS < 16K
 *  0- 7: locked byte
 *     8: pending
 *  9-15: not used
 * 16-17: tail index
 * 18-31: tail cpu (+1)
 *
 * When NR_CPUS >= 16K
 *  0- 7: locked byte
 *     8: pending
 *  9-10: tail index
 * 11-31: tail cpu (+1)
 *
 * |<---1--->|<---8--->|<---2--->|<---others--->|
 *    value   pending     idx         cpu
 */

#define _Q_PENDING_OFFSET	(_Q_LOCKED_OFFSET + _Q_LOCKED_BITS)
#define _Q_PENDING_BITS		8
//#define _Q_PENDING_BITS		1
#define _Q_PENDING_MASK		_Q_SET_MASK(PENDING)

#define _Q_TAIL_IDX_OFFSET	(_Q_PENDING_OFFSET + _Q_PENDING_BITS)
#define _Q_TAIL_IDX_BITS	2
#define _Q_TAIL_IDX_MASK	_Q_SET_MASK(TAIL_IDX)

#define _Q_TAIL_CPU_OFFSET	(_Q_TAIL_IDX_OFFSET + _Q_TAIL_IDX_BITS)
#define _Q_TAIL_CPU_BITS	(32 - _Q_TAIL_CPU_OFFSET)
#define _Q_TAIL_CPU_MASK	_Q_SET_MASK(TAIL_CPU)

#define _Q_TAIL_OFFSET		_Q_TAIL_IDX_OFFSET
#define _Q_TAIL_MASK		(_Q_TAIL_IDX_MASK | _Q_TAIL_CPU_MASK)

#define _Q_PENDING_VAL		(1U << _Q_PENDING_OFFSET)
#define _Q_LOCKED_PENDING_MASK (_Q_LOCKED_MASK | _Q_PENDING_MASK)
////////////////////////////////////////////////////////////////////////////////
/*
 * By using the whole 2nd least significant byte for the pending bit, we
 * can allow better optimization of the lock acquisition for the pending
 * bit holder.
 *
 * This internal structure is also used by the set_locked function which
 * is not restricted to _Q_PENDING_BITS == 8.
 */
struct __qspinlock {
	union {
		atomic_t val;
#ifdef __LITTLE_ENDIAN
		struct {
			uint8_t	locked;
			uint8_t	pending;
		};
		struct {
			uint16_t locked_pending;
			uint16_t tail;
		};
#else
		struct {
			uint16_t tail;
			uint16_t locked_pending;
		};
		struct {
			uint8_t	reserved[2];
			uint8_t	pending;
			uint8_t	locked;
		};
#endif
	};
};

#if _Q_PENDING_BITS == 8
/**
 * clear_pending_set_locked - take ownership and clear the pending bit.
 * @lock: Pointer to queued spinlock structure
 *
 * *,1,0 -> *,0,1
 *
 * Lock stealing is not allowed if this function is used.
 */
static __always_inline void clear_pending_set_locked(spinlock_t *lock)
{
	struct __qspinlock *l = (void *)&lock->raw_lock;

	WRITE_ONCE(l->locked_pending, _Q_LOCKED_VAL);
}

#else /* _Q_PENDING_BITS == 8 */

/**
 * clear_pending_set_locked - take ownership and clear the pending bit.
 * @lock: Pointer to queued spinlock structure
 *
 * *,1,0 -> *,0,1
 */
static __always_inline void clear_pending_set_locked(spinlock_t *lock)
{
	atomic_add(-_Q_PENDING_VAL + _Q_LOCKED_VAL, &lock->raw_lock);
}

#endif /* _Q_PENDING_BITS == 8 */

/**
 * queued_spin_lock_slowpath - acquire the queued spinlock
 * @lock: Pointer to queued spinlock structure
 * @val: Current value of the queued spinlock 32-bit word
 *
 * (queue tail, pending bit, lock value)
 *
 *              fast     :    slow                                  :    unlock
 *                       :                                          :
 * uncontended  (0,0,0) -:--> (0,0,1) ------------------------------:--> (*,*,0)
 *                       :       | ^--------.------.             /  :
 *                       :       v           \      \            |  :
 * pending               :    (0,1,1) +--> (0,1,0)   \           |  :
 *                       :       | ^--'              |           |  :
 *                       :       v                   |           |  :
 * uncontended           :    (n,x,y) +--> (n,0,0) --'           |  :
 *   queue               :       | ^--'                          |  :
 *                       :       v                               |  :
 * contended             :    (*,x,y) +--> (*,0,0) ---> (*,0,1) -'  :
 *   queue               :         ^--'                             :
 */

__thread unsigned long atomic_ctx = 0;
void queued_spin_lock_slowpath(spinlock_t *lock, uint32_t val)
{
	uint32_t _new_, old;

try:
	/*
	 * wait for in-progress pending->locked hand-overs
	 * 有人正在排队
	 *
	 * 0,1,0 -> 0,0,1
	 */
	if (val == _Q_PENDING_VAL) {
		__cond_load_acquire(&lock->raw_lock.counter,
				!((val = VAL) == _Q_PENDING_VAL));
	}

	/*
	 * trylock || pending
	 *
	 * 0,0,0 -> 0,0,1 ; trylock 可以尝试加锁
	 * 0,0,1 -> 0,1,1 ; pending 否则尝试标志为未决状态
	 */
	for (;;) {
		/*
		 * If we observe any contention; queue.
		 * 其他路径已经标记则排队
		 */
		if (val & ~ _Q_LOCKED_MASK)
			goto queue;

		/*尝试加锁位并添加未决标记*/
		_new_ = _Q_LOCKED_VAL;
		if (val == _new_) /*其他人已经加锁，则仅标记*/
			_new_ |= _Q_PENDING_VAL;

		/*
		 * Acquire semantic is required here as the function may
		 * return immediately if the lock was free.
		 */
		old = atomic_cmpxchg(&lock->raw_lock, val, _new_);
		if (old == val)
			break;

		val = old;
	}

	/*
	 * we won the trylock
	 * 仅尝试加锁，并且成功了
	 */
	if (_new_ == _Q_LOCKED_VAL)
		return;

	/*
	 * we're pending, wait for the owner to go away.
	 * 现在已经获取了未决标志，等待他人解锁
	 *
	 * *,1,1 -> *,1,0
	 *
	 * this wait loop must be a load-acquire such that we match the
	 * store-release that clears the locked bit and create lock
	 * sequentiality; this is because not all clear_pending_set_locked()
	 * implementations imply full barriers.
	 */
	__cond_load_acquire(&lock->raw_lock.counter,
			!(VAL & _Q_LOCKED_MASK));

	/*
	 * take ownership and clear the pending bit.
	 *
	 * *,1,0 -> *,0,1
	 */
	clear_pending_set_locked(lock);

	return;
	/*
	 * End of pending bit optimistic spinning and beginning of MCS
	 * queuing.
	 */
queue:
	/*
	 * We touched a (possibly) cold cacheline in the per-cpu queue node;
	 * attempt the trylock once more in the hope someone let go while we
	 * weren't watching.
	 */
	__cond_load_acquire(&lock->raw_lock.counter,
		!(VAL & _Q_PENDING_MASK));
	val = atomic_cmpxchg(&lock->raw_lock, 0, _Q_LOCKED_VAL);
	if (skp_likely(!val))
		return;
	goto try;
}

void __queued_spin_lock_fast(spinlock_t *lock)
{
	uint32_t val;
	int32_t i = __SPINLOCK_MAX_CONTENDS;
	do {
		val = atomic_cmpxchg(&lock->raw_lock, 0, _Q_LOCKED_VAL);
		if (skp_likely(!val))
			return;
		__cond_load_acquire(&lock->raw_lock.counter,
				!(VAL & _Q_LOCKED_MASK));
	} while (skp_likely(--i > 0));
	queued_spin_lock_slowpath(lock, val);
}
