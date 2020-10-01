#ifndef __US_UREF_H__
#define __US_UREF_H__

#include "utils.h"
#include "mutex.h"

__BEGIN_DECLS

struct uref {
	atomic_t refcount;
};

typedef struct uref uref_t;

#define UREF_INIT(n) { .refcount = ATOMIC_INIT(n), }

static inline void uref_init(uref_t *uref)
{
	atomic_set(&uref->refcount, 1);
}

static inline void uref_set(uref_t *uref, int cnt)
{
	atomic_set(&uref->refcount, cnt);
}

static inline uint32_t uref_read(const uref_t *uref)
{
	return atomic_read(&uref->refcount);
}

static inline void uref_get(uref_t *uref)
{
	atomic_inc(&uref->refcount);
}

static inline bool uref_put(uref_t *uref, void (*release)(uref_t *uref))
{
	if (atomic_dec_and_test(&uref->refcount)) {
		release(uref);
		return true;
	}
	return false;
}

/*@return true then dec ref to zero and hold lock*/
static inline bool uref_put_lock(uref_t *uref, void (*release)(uref_t *uref),
		spinlock_t *lock)
{
	if (atomic_dec_and_lock(&uref->refcount, lock)) {
		release(uref);
		return true;
	}
	return false;
}

static inline bool uref_put_mutex(uref_t *uref, void (*release)(uref_t *uref),
		mutex_t *lock)
{
	if (atomic_dec_and_mutex(&uref->refcount, lock)) {
		release(uref);
		return true;
	}
	return false;
}

static __always_inline bool __uref_add_not_zero(unsigned int i, uref_t *uref)
{
	int r, c = atomic_read(&uref->refcount);
	do {
		if (skp_unlikely(c == 0))
			return false;
		r = c + i;
		if (skp_unlikely(c < 0 || c == INT_MAX || r < c)) {
			/*引用计数被破坏*/
			BUG();
		}
	} while (!atomic_try_cmpxchg(&uref->refcount, &c, r));
	return !!(c != 0);
}

static inline bool uref_get_unless_zero(uref_t *uref)
{
	return __uref_add_not_zero(1, uref);
}

static inline bool __uref_put(uref_t *uref)
{
	if (atomic_dec_and_test(&uref->refcount))
		return true;
	return false;
}

/*@return true then dec ref to zero and hold lock*/
static inline bool __uref_put_lock(uref_t *uref, spinlock_t *lock)
{
	if (atomic_dec_and_lock(&uref->refcount, lock))
		return true;
	return false;
}

static inline bool __uref_put_mutex(uref_t *uref, mutex_t *lock)
{
	if (atomic_dec_and_mutex(&uref->refcount, lock))
		return true;
	return false;
}

__END_DECLS

#endif
