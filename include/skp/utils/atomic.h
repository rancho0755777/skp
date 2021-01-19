#ifndef __US_ATOMIC_H__
#define __US_ATOMIC_H__

#include <sched.h>
#include "bitops.h"

__BEGIN_DECLS

typedef struct _atomic {
	int counter;
} atomic_t;

typedef struct _atomic64 {
	long long counter;
} atomic64_t;

#define ATOMIC_INIT(i) { (i) }
#define ATOMIC64_INIT(i) { (i) }

extern int __atomic_wrong_size(volatile void *, ...)
	__compiletime_error("Bad argument size for atomic operation");

#include "atomic64_64.h"

#define cmpxchg8(p, o, n) __sync_bool_compare_and_swap(						\
				(volatile uint8_t*)(p), (uint8_t)(o), (uint8_t)(n))
#define cmpxchg16(p, o, n) __sync_bool_compare_and_swap(					\
				(volatile uint16_t*)(p), (uint16_t)(o), (uint16_t)(n))
#define cmpxchg32(p, o, n) __sync_bool_compare_and_swap(					\
				(volatile uint32_t*)(p), (uint32_t)(o), (uint32_t)(n))
#if BITS_PER_LONG == 64
# define cmpxchg64(p, o, n) __sync_bool_compare_and_swap(					\
				(volatile uint64_t*)(p), (uint64_t)(o), (uint64_t)(n))
#else
extern bool cmpxchg64(volatile uint64_t *ptr, uint64_t old, uint64_t _new);
#endif

#define cmpxchg8_val(p, o, n)  __sync_val_compare_and_swap(					\
				(volatile uint8_t*)(p), (uint8_t)(o), (uint8_t)(n))
#define cmpxchg16_val(p, o, n) __sync_val_compare_and_swap(					\
				(volatile uint16_t*)(p), (uint16_t)(o), (uint16_t)(n))
#define cmpxchg32_val(p, o, n) __sync_val_compare_and_swap(					\
				(volatile uint32_t*)(p), (uint32_t)(o), (uint32_t)(n))
#if BITS_PER_LONG == 64
# define cmpxchg64_val(p, o, n) __sync_val_compare_and_swap(				\
				(volatile uint64_t*)(p), (uint64_t)(o), (uint64_t)(n))
#else
extern uint64_t cmpxchg64_val(volatile uint64_t *, uint64_t old, uint64_t _new);
#endif

#define xchg8(p, n) __sync_lock_test_and_set((volatile uint8_t*)(p),		\
				(uint8_t)(n))
#define xchg16(p, n) __sync_lock_test_and_set((volatile uint16_t*)(p),		\
				(uint16_t)(n))
#define xchg32(p, n) __sync_lock_test_and_set((volatile uint32_t*)(p),		\
				(uint32_t)(n))
#if BITS_PER_LONG == 64
# define xchg64(p, n) __sync_lock_test_and_set((volatile uint64_t*)(p),		\
				(uint64_t)(n))
#else
extern uint64_t xchg64(volatile uint64_t *ptr, uint64_t _new);
#endif

#define xadd8(p, n) __sync_fetch_and_add((volatile uint8_t*)(p),(uint8_t)(n))
#define xadd16(p, n) __sync_fetch_and_add((volatile uint16_t*)(p),(uint16_t)(n))
#define xadd32(p, n) __sync_fetch_and_add((volatile uint32_t*)(p),(uint32_t)(n))
#if BITS_PER_LONG == 64
# define xadd64(p, n) __sync_fetch_and_add((uint64_t*)(p), (uint64_t)(n))
#else
extern uint64_t xadd64(volatile uint64_t *ptr, uint64_t _new);
#endif

#define __aligned_double_cmpxchg __aligned(sizeof(long) * 2)

#define cmpxchg_double(p1, p2, o1, o2, n1, n2)								\
({																			\
	bool __ret;																\
	__typeof__(*(p1)) __old1 = (o1), __new1 = (n1);							\
	__typeof__(*(p2)) __old2 = (o2), __new2 = (n2);							\
	BUILD_BUG_ON(sizeof(*(p1)) != sizeof(long));							\
	BUILD_BUG_ON(sizeof(*(p2)) != sizeof(long));							\
	BUG_ON((unsigned long)(p1) % (2 * sizeof(long)));						\
	BUG_ON((unsigned long)((p1) + 1) != (unsigned long)(p2));				\
	asm volatile(LOCK_PREFIX "cmpxchg%c4b %2; sete %0"						\
		     : "=a" (__ret), "+d" (__old2),									\
		       "+m" (*(p1)), "+m" (*(p2))									\
		     : "i" (2 * sizeof(long)), "a" (__old1),						\
		       "b" (__new1), "c" (__new2));									\
	__ret;																	\
})

static inline void atomic_add(int i, atomic_t *v)
{
	asm volatile(LOCK_PREFIX "addl %1,%0" : "+m" (v->counter) : "ir" (i));
}

/**
 * Atomically subtracts @i from @v and returns
 * true if the result is zero, or false for all
 * other cases.
 */
static inline bool atomic_sub_and_test(int i, atomic_t *v)
{
	GEN_BINARY_RMWcc(LOCK_PREFIX "subl", v->counter, "er", i, "%0", e);
}

/**
 * Atomically decrements @v by 1 and
 * returns true if the result is 0, or false for all other cases.
 */
static inline bool atomic_dec_and_test(atomic_t *v)
{
	GEN_UNARY_RMWcc(LOCK_PREFIX "decl", v->counter, "%0", e);
}

/**
 * Atomically increments @v by 1
 * and returns true if the result is zero, or false for all other cases.
 */
static inline bool atomic_inc_and_test(atomic_t *v)
{
	GEN_UNARY_RMWcc(LOCK_PREFIX "incl", v->counter, "%0", e);
}

/**
 * Atomically adds @i to @v and returns true if the result is negative, 
 * or false when result is greater than or equal to zero.
 */
static inline bool atomic_add_negative(int i, atomic_t *v)
{
	GEN_BINARY_RMWcc(LOCK_PREFIX "addl", v->counter, "er", i, "%0", s);
}

static inline void atomic_and(int i, atomic_t *v)
{
	asm volatile(LOCK_PREFIX "andl %1,%0":"+m"(v->counter):"ir"(i):"memory");
}

static inline void atomic_or(int i, atomic_t *v)
{
	asm volatile(LOCK_PREFIX "orl %1,%0":"+m"(v->counter):"ir"(i):"memory");
}

static inline void atomic_xor(int i, atomic_t *v)
{
	asm volatile(LOCK_PREFIX "xorl %1,%0":"+m"(v->counter):"ir"(i):"memory");
}

#define xadd(p, n)															\
({ 																			\
	typeof(*(p)) __val; 													\
	typeof(p) __p = (p); 													\
	switch(sizeof(__val)) {													\
		case 1 : __val = xadd8(__p, (n)); break;							\
		case 2 : __val = xadd16(__p, (n)); break;							\
		case 4 : __val = xadd32(__p, (n)); break;							\
		case 8 : __val = xadd64(__p, (n)); break;							\
		default : __atomic_wrong_size(__p, (n));							\
	} 																		\
	__val; 																	\
})

#define xchg(p, n)															\
({ 																			\
	typeof(*(p)) __val; 													\
	typeof(p) __p = (p); 													\
	switch(sizeof(__val)) {													\
		case 1 : __val = xchg8(__p, (n)); break;							\
		case 2 : __val = xchg16(__p, (n)); break;							\
		case 4 : __val = xchg32(__p, (n)); break;							\
		case 8 : __val = xchg64(__p, (n)); break;							\
		default : __atomic_wrong_size(__p, (n)); 							\
	} 																		\
	__val; 																	\
})

#define cmpxchg(p, o, n)													\
({ 																			\
	bool __val; 															\
	typeof(p) __p = (p); 													\
	switch(sizeof(*__p)) {													\
		case 1 : __val = cmpxchg8(__p, (o), (n)); break;					\
		case 2 : __val = cmpxchg16(__p, (o), (n)); break;					\
		case 4 : __val = cmpxchg32(__p, (o), (n)); break;					\
		case 8 : __val = cmpxchg64(__p, (o), (n)); break;					\
		default : __atomic_wrong_size(__p, (o), (n)); 						\
	} 																		\
	__val; 																	\
})

#define cmpxchg_val(p, o, n)												\
({ 																			\
	typeof(*(p)) __val; 													\
	typeof(p) __p = (p); 													\
	switch(sizeof(__val)) {													\
		case 1 : __val = cmpxchg8_val(__p, (o), (n)); break;				\
		case 2 : __val = cmpxchg16_val(__p, (o), (n)); break;				\
		case 4 : __val = cmpxchg32_val(__p, (o), (n)); break;				\
		case 8 : __val = cmpxchg64_val(__p, (o), (n)); break;				\
		default : __atomic_wrong_size(__p, (o), (n)); 						\
	} 																		\
	__val; 																	\
})

#define try_cmpxchg(_p, _po, _n)											\
({																			\
	typeof(_po) __po = (_po);												\
	typeof(*(_po)) __r, __o = *__po;										\
	__r = cmpxchg_val((_p), __o, (_n));										\
	if (skp_unlikely(__r != __o))											\
		*__po = __r;														\
	skp_likely(__r == __o);													\
})

#define __xadd_unless(ptr, v, u) 											\
({																			\
	typeof(ptr) __ptr = (ptr);												\
	typeof(*(ptr)) __o = READ_ONCE(*__ptr),									\
		__v = (v), __u = (u); 												\
	do { 																	\
		if (skp_unlikely(__o == __u)) break;								\
	} while (!try_cmpxchg(__ptr, &__o, __o + __v)); 						\
	__o; 																	\
})

#define xadd_unless(ptr, v, u)												\
({	typeof(u) __cu = (u);													\
	__xadd_unless((ptr), (v), __cu) != __cu; 								\
})

#if BITS_PER_LONG == 64
#define xchg_ptr(p, n)														\
	((void*)(uintptr_t)xchg64((uintptr_t)(p), (uintptr_t)(n)))
#define cmpxchg_ptr(p, o, n)												\
	(cmpxchg64_val((uintptr_t)(p), (uintptr_t)(o), (uintptr_t)(n)))
#define cmpxchg_val_ptr(p, o, n)											\
	((void*)(uintptr_t)cmpxchg64_val((uintptr_t)(p),(uintptr_t)(o),			\
		(uintptr_t)(n)))
#else
#define xchg_ptr(p, n)														\
	((void*)(uintptr_t)xchg32((uintptr_t)(p), (n)))
#define cmpxchg_ptr(p, o, n)												\
	(cmpxchg32_val((uintptr_t)(p), (uintptr_t)(o), (uintptr_t)(n)))
#define cmpxchg_val_ptr(p, o, n)											\
	((void*)(uintptr_t)cmpxchg32_val((uintptr_t)(p), (uintptr_t)(o),		\
		(uintptr_t)(n)))
#endif

static inline int atomic_read(const atomic_t *v)
{
	return READ_ONCE((v)->counter);
}

static inline void atomic_set(atomic_t *v, int i)
{
	WRITE_ONCE(v->counter, i);
}

/*返回新值*/
static inline int atomic_add_return(int i, atomic_t *v)
{
	return i + xadd(&v->counter, i);
}

static inline int atomic_return_add(int i, atomic_t *v)
{
	return xadd(&v->counter, i);
}

static inline int atomic_cmpxchg(atomic_t *v, int old, int _new_)
{
	return cmpxchg_val(&v->counter, old, _new_);
}

static inline bool atomic_try_cmpxchg(atomic_t *v, int *old, int _new_)
{
	return try_cmpxchg(&v->counter, old, _new_);
}

static inline int atomic_xchg(atomic_t *v, int _new_)
{
	return xchg(&v->counter, _new_);
}

static __always_inline int atomic_return_and(int i, atomic_t *v)
{
	int val = atomic_read(v);
	do { } while (!atomic_try_cmpxchg(v, &val, val & i));
	return val;
}

static __always_inline int atomic_return_or(int i, atomic_t *v)
{
	int val = atomic_read(v);
	do { } while (!atomic_try_cmpxchg(v, &val, val | i));
	return val;
}

static __always_inline int atomic_return_xor(int i, atomic_t *v)
{
	int val = atomic_read(v);
	do { } while (!atomic_try_cmpxchg(v, &val, val ^ i));
	return val;
}

/**
 * Atomically adds @a to @v, so long as @v was not already @u.
 * Returns the old value of @v.
 */
static __always_inline int __atomic_add_unless(atomic_t *v, int a, int u)
{
	return __xadd_unless(&v->counter, a, u);
}

/**
 * Atomically adds a to v, so long as @v was not already @u.
 * Returns true if v was not u, and zero otherwise.
 * 不等于*v != u 则 v-=a 并返回真，否则不进行加运算，并返回假。
 */
static inline bool atomic_add_unless(atomic_t *v, int a, int u)
{
	return __atomic_add_unless(v, a, u) != u;
}

/*increment unless the atomic is negative
 * 原子递增非负数
 */
static __always_inline int atomic_inc_unless_negative(atomic_t *p)
{
	int v, v1;
	for (v = 0; v >= 0; v = v1) {
		v1 = atomic_cmpxchg(p, v, v + 1);
		if (skp_likely(v1 == v))
			return 1;
	}
	return 0;
}

/*decrement unless the atomic is positive
 * 原子递减非正数
 */
static __always_inline int atomic_dec_unless_positive(atomic_t *p)
{
	int v, v1;
	for (v = 0; v <= 0; v = v1) {
		v1 = atomic_cmpxchg(p, v, v - 1);
		if (skp_likely(v1 == v))
			return 1;
	}
	return 0;
}

/*
 * 原子递减正数
 */
static __always_inline int atomic_dec_if_positive(atomic_t *v)
{
	int c, old, dec;
	c = atomic_read(v);
	for (;;) {
		dec = c - 1;
		if (skp_unlikely(dec < 0))
			break;
		old = atomic_cmpxchg((v), c, dec);
		if (skp_likely(old == c))
			break;
		c = old;
	}
	return dec;
}

#define atomic_sub(i, v) atomic_add(-(i), (v))
#define atomic_inc(v) atomic_add(1, (v))
#define atomic_dec(v) atomic_add(-1, (v))

#define atomic_sub_return(i, v) atomic_add_return(-(i), (v))
#define atomic_inc_return(v)  atomic_add_return(1, v)
#define atomic_dec_return(v)  atomic_sub_return(1, v)

#define atomic_return_sub(i, v) atomic_return_add(-(i), (v))
#define atomic_return_inc(v)	atomic_return_add(1, (v))
#define atomic_return_dec(v) atomic_return_add(-1, (v))

/*a &= ~value*/
#define atomic_andnot(i, v) atomic_and(~(i), (v))
#define atomic_return_andnot(i, v) atomic_return_and(~(i), (v))

/**
 * Atomically increments @v by 1, so long as @v is non-zero.
 * Returns true if @v was non-zero, and zero otherwise.
 */
#define atomic_inc_not_zero(v) atomic_add_unless((v), 1, 0)
////////////////////////////////////////////////////////////////////////////////
/*返回新值*/
static inline long long atomic64_add_return(long long i, atomic64_t *v)
{
	return i + xadd(&v->counter, i);
}
static inline long long atomic64_fetch_add(long long i, atomic64_t *v)
{
	return xadd(&v->counter, i);
}

static inline long long atomi64_fetch_sub(long long i, atomic64_t *v)
{
	return xadd(&v->counter, -i);
}

static inline long long atomic64_cmpxchg(atomic64_t *v, long long old,
		long long _new_)
{
	return cmpxchg_val(&v->counter, old, _new_);
}

static inline bool atomic64_try_cmpxchg(atomic64_t *v, long long *old,
		long long _new_)
{
	long long oval = *old, ret = cmpxchg_val(&v->counter, *old, _new_);
	if (skp_unlikely(ret != oval))
		*old = ret;
	return !!(ret == oval);
}

static inline long long atomic64_xchg(atomic64_t *v, long long _new_)
{
	return xchg(&v->counter, _new_);
}

static inline long long atomic64_fetch_and(long long i, atomic64_t *v)
{
	long long val = atomic64_read(v);
	do { } while (!atomic64_try_cmpxchg(v, &val, val & i));
	return val;
}

static inline long long atomic64_fetch_or(long long i, atomic64_t *v)
{
	long long val = atomic64_read(v);
	do { } while (!atomic64_try_cmpxchg(v, &val, val | i));
	return val;
}

static inline long long atomic64_fetch_xor(long long i, atomic64_t *v)
{
	long long val = atomic64_read(v);
	do { } while (!atomic64_try_cmpxchg(v, &val, val ^ i));
	return val;
}

/**
 * Atomically adds @a to @v, so long as @v was not already @u.
 * Returns the old value of @v.
 */
static inline long long __atomic64_add_unless(atomic64_t *v, long long a, long long u)
{
	long long c = atomic64_read(v);
	do {
		if (skp_unlikely(c == u))
			break;
	} while (!atomic64_try_cmpxchg(v, &c, c + a));
	return c;
}

/**
 * Atomically adds a to v, so long as @v was not already @u.
 * Returns true if v was not u, and zero otherwise.
 * 不等于*v != u 则 v-=a 并返回真，否则不进行加运算，并返回假。
 */
static inline bool atomic64_add_unless(atomic64_t *v, long long a, long long u)
{
	return __atomic64_add_unless(v, a, u) != u;
}

/*increment unless the atomic64 is negative
 * 原子递增非负数
 */
static __always_inline long long atomic64_inc_unless_negative(atomic64_t *p)
{
	long long v, v1;
	for (v = 0; v >= 0; v = v1) {
		v1 = atomic64_cmpxchg(p, v, v + 1);
		if (skp_likely(v1 == v))
			return 1;
	}
	return 0;
}

/*decrement unless the atomic64 is positive
 * 原子递减非正数
 */
static __always_inline long long atomic64_dec_unless_positive(atomic64_t *p)
{
	long long v, v1;
	for (v = 0; v <= 0; v = v1) {
		v1 = atomic64_cmpxchg(p, v, v - 1);
		if (skp_likely(v1 == v))
			return 1;
	}
	return 0;
}

/*
 * atomic_dec_if_positive - decrement by 1 if old value positive
 * The function returns the old value of *v minus 1, even if
 * the atomic variable, v, was not decremented, but if it's negative then
 * atomic variable was decremented already.
 * 原子递减正数
 */
static __always_inline long long atomic64_dec_if_positive(atomic64_t *v)
{
	long long c, old, dec;
	c = atomic64_read(v);
	for (;;) {
		dec = c - 1;
		if (skp_unlikely(dec < 0))
			break;
		old = atomic64_cmpxchg((v), c, dec);
		if (skp_likely(old == c))
			break;
		c = old;
	}
	return dec;
}

#define atomic64_sub(i, v) atomic64_add(-(i), (v))
#define atomic64_inc(v) atomic64_add(1, (v))
#define atomic64_dec(v) atomic64_add(-1, (v))

#define atomic64_sub_return(i, v) atomic64_add_return(-(i), (v))
#define atomic64_inc_return(v)  atomic64_add_return(1, v)
#define atomic64_dec_return(v)  atomic64_sub_return(1, v)

#define atomic64_fetch_sub(i, v) atomic64_fetch_add(-(i), (v))
#define atomic64_fetch_inc(v)	atomic64_fetch_add(1, (v))
#define atomic64_fetch_dec(v) atomic64_fetch_add(-1, (v))

/*a &= ~value*/
#define atomic64_andnot(i, v) atomic64_and(~(i), (v))
#define atomic64_fetch_andnot(i, v) atomic64_fetch_and(~(i), (v))
/**
 * Atomically increments @v by 1, so long as @v is non-zero.
 * Returns true if @v was non-zero, and zero otherwise.
 */
#define atomic64_inc_not_zero(v)		atomic64_add_unless((v), 1, 0)
/////////////////////////////////////////////////////////////////////////////////
#define __store_release(p, v) \
	do { compiletime_assert_atomic_type(*p); static_mb();  WRITE_ONCE(*p, v); } while (0)

/**
 * __cond_load_acquire() - (Spin) wait for cond with ACQUIRE ordering
 * @ptr: pointer to the variable to wait on
 * @cond: boolean expression to wait for
 */
#define __cond_load_acquire(ptr, cond_expr) \
do {										\
	typeof(ptr) __PTR = (ptr);				\
	typeof(*ptr) VAL;						\
	for (uint32_t __seq = 1;;__seq++) {		\
		VAL = READ_ONCE(*__PTR);			\
		if (cond_expr)						\
			break;							\
		if (skp_likely(__seq & 3)) {		\
			cpu_relax();					\
		} else { 							\
			sched_yield();					\
		}									\
	}										\
	static_mb();							\
} while (0)
/////////////////////////////////////////////////////////////////////////////////


__END_DECLS

#endif
