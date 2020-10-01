#ifndef __US_ATOMIC64_64_H__
#define __US_ATOMIC64_64_H__

#include "bitops.h"

#ifndef __US_ATOMIC_H__
#error only "atomic.h" can be included directly
#endif
/* The 64-bit atomic type */

__BEGIN_DECLS

#if BITS_PER_LONG == 64
static inline long long atomic64_read(const atomic64_t *v)
{
	return READ_ONCE((v)->counter);
}
static inline void atomic64_set(atomic64_t *v, long long i)
{
	WRITE_ONCE(v->counter, i);
}
static inline void atomic64_add(long long i, atomic64_t *v)
{
	asm volatile(LOCK_PREFIX "addq %1,%0" : "=m" (v->counter) : "er" (i), "m" (v->counter));
}
static inline bool atomic64_sub_and_test(long long i, atomic64_t *v)
{
	GEN_BINARY_RMWcc(LOCK_PREFIX "subq", v->counter, "er", i, "%0", e);
}
static inline bool atomic64_dec_and_test(atomic64_t *v)
{
	GEN_UNARY_RMWcc(LOCK_PREFIX "decq", v->counter, "%0", e);
}
static inline bool atomic64_inc_and_test(atomic64_t *v)
{
	GEN_UNARY_RMWcc(LOCK_PREFIX "incq", v->counter, "%0", e);
}
static inline bool atomic64_add_negative(long long i, atomic64_t *v)
{
	GEN_BINARY_RMWcc(LOCK_PREFIX "addq", v->counter, "er", i, "%0", s);
}
static inline void atomic64_and(long long i, atomic64_t *v)
{
	asm volatile(LOCK_PREFIX "andq %1,%0" : "+m" (v->counter) : "er" (i) : "memory");
}
static inline void atomic64_or(long long i, atomic64_t *v)
{
	asm volatile(LOCK_PREFIX "orq %1,%0" : "+m" (v->counter) : "er" (i) : "memory");
}
static inline void atomic64_xor(long long i, atomic64_t *v)
{
	asm volatile(LOCK_PREFIX "xorq %1,%0" : "+m" (v->counter) : "er" (i) : "memory");
}
#else
extern long long atomic64_read(const atomic64_t *v);
extern void atomic64_set(atomic64_t *v, long long i);
extern void atomic64_add(long long i, atomic64_t *v);
extern bool atomic64_sub_and_test(long long i, atomic64_t *v);
extern bool atomic64_add_negative(long long i, atomic64_t *v);
extern void atomic64_and(long long i, atomic64_t *v);
extern void atomic64_or(long long i, atomic64_t *v);
extern void atomic64_xor(long long i, atomic64_t *v);

static inline bool atomic64_dec_and_test(atomic64_t *v)
{
	return atomic64_sub_and_test(1, v);
}

static inline bool atomic64_inc_and_test(atomic64_t *v)
{
	return atomic64_sub_and_test(-1, v);
}

#endif

__END_DECLS

#endif
