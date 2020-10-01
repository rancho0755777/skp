#include <skp/utils/bitops.h>

#if BITS_PER_LONG == 32
# include <skp/utils/hash.h>
# include <skp/utils/atomic.h>
# include <skp/utils/spinlock.h>
static unsigned long op64_bitlock = 0;

static inline void op64lock(uint64_t *addr)
{
	bit_spin_lock(hash_ptr(addr, BITS_PER_LONG_SHIFT), &op64_bitlock);
}

static inline void op64unlock(uint64_t *addr)
{
	bit_spin_unlock(hash_ptr(addr, BITS_PER_LONG_SHIFT), &op64_bitlock);
}

void __read_once_size__(const volatile void *p, void *res, size_t size)
{
#ifndef __x86_64__
	BUILD_BUG_ON(BITS_PER_LONG == 64);
#endif
	switch (size) {
		case 8:
			op64lock(p);
			*(uint64_t*)res = *(volatile uint64_t*)p;
			op64unlock(p);
			break;
		default:
			static_mb();
			__builtin_memcpy((void *)res, (const void *)p, size);
			break;
	}
}

void __write_once_size__(volatile void *p, void *res, size_t size)
{
#ifndef __x86_64__
	BUILD_BUG_ON(BITS_PER_LONG == 64);
#endif
	switch (size) {
		case 8:
			op64lock(p);
			*(volatile uint64_t *)p = *(uint64_t *)res;
			op64unlock(p);
			break;
		default:
			static_mb();
			__builtin_memcpy((void *)p, (const void *)res, size);
			break;
	}
}

long long atomic64_read(const atomic64_t *v)
{
	long long r64;
	op64lock(v);
	r64 = v->counter;
	op64unlock(v);
	return r64;
}

void atomic64_set(atomic64_t *v, long long i)
{
	op64lock(v);
	v->counter = i;
	op64unlock(v);
}

void atomic64_add(long long i, atomic64_t *v)
{
	op64lock(v);
	v->counter += i;
	op64unlock(v);
}

bool atomic64_sub_and_test(long long i, atomic64_t *v)
{
	bool r;
	op64lock(v);
	v->counter -= i;
	static_mb();
	r = !v->counter;
	op64unlock(v);
	return r;
}

bool atomic64_add_negative(long long i, atomic64_t *v)
{
	bool r;
	op64lock(v);
	v->counter -= i;
	static_mb();
	r = !!(v->counter < 0);
	op64unlock(v);
	return r;
}

void atomic64_and(long long i, atomic64_t *v)
{
	op64lock(v);
	v->counter &= i;
	op64unlock(v);
}

void atomic64_or(long long i, atomic64_t *v)
{
	op64lock(v);
	v->counter |= i;
	op64unlock(v);
}

void atomic64_xor(long long i, atomic64_t *v)
{
	op64lock(v);
	v->counter ^= i;
	op64unlock(v);
}

bool cmpxchg64(volatile uint64_t *ptr, uint64_t old, uint64_t new)
{
	bool r = false;
	op64lock(ptr);
	if (*ptr == old) {
		*ptr = new;
		r = true;
	}
	op64unlock(ptr);
	return r;
}

uint64_t cmpxchg64_val(volatile uint64_t *ptr, uint64_t old, uint64_t new)
{
	uint64_t r64 = old;
	op64lock(ptr);
	if (skp_likely(*ptr == old)) {
		*ptr = new;
	} else {
		r64 = *ptr;
	}
	op64unlock(ptr);
	return r64;
}

uint64_t xchg64(volatile uint64_t *ptr, uint64_t new)
{
	uint64_t r64;
	op64lock(ptr);
	r64 = ptr->counter;
	static_mb();
	ptr->counter = new;
	op64unlock(ptr);
	return r64;
}

uint64_t xadd64(volatile uint64_t *ptr, uint64_t new)
{
	uint64_t r64;
	op64lock(ptr);
	r64 = ptr->counter;
	static_mb();
	ptr->counter += new;
	op64unlock(ptr);
	return r64;
}
#else
void __read_once_size__(const volatile void *p, void *res, size_t size)
{
	static_mb();
	__builtin_memcpy((void *)res, (const void *)p, size);
}
void __write_once_size__(volatile void *p, void *res, size_t size)
{
	static_mb();
	__builtin_memcpy((void *)p, (const void *)res, size);
}
#endif
