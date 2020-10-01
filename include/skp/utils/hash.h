#ifndef __US_HASH_H__
#define __US_HASH_H__

/* Fast hashing routine for ints,  longs and pointers.
   (C) 2002 Nadia Yvette Chambers, IBM */

#include "bitops.h"

__BEGIN_DECLS

static inline uint32_t __get_unaligned_cpu32(const void *p)
{
	struct __una_u32 { uint32_t x; } __aligned_packed;
	const struct __una_u32 *ptr = (const struct __una_u32 *)p;
	return ptr->x;
}

/* Best hash sizes are of power of two */
#define jhash_size(n)   ((uint32_t)1<<(n))
/* Mask the hash value, i.e (value & jhash_mask(n)) instead of (value % n) */
#define jhash_mask(n)   (jhash_size(n)-1)

/* __jhash_mix -- mix 3 32-bit values reversibly. */
#define __jhash_mix(a, b, c)			\
{						\
	a -= c;  a ^= rol32(c, 4);  c += b;	\
	b -= a;  b ^= rol32(a, 6);  a += c;	\
	c -= b;  c ^= rol32(b, 8);  b += a;	\
	a -= c;  a ^= rol32(c, 16); c += b;	\
	b -= a;  b ^= rol32(a, 19); a += c;	\
	c -= b;  c ^= rol32(b, 4);  b += a;	\
}

/* __jhash_final - final mixing of 3 32-bit values (a,b,c) into c */
#define __jhash_final(a, b, c)			\
{						\
	c ^= b; c -= rol32(b, 14);		\
	a ^= c; a -= rol32(c, 11);		\
	b ^= a; b -= rol32(a, 25);		\
	c ^= b; c -= rol32(b, 16);		\
	a ^= c; a -= rol32(c, 4);		\
	b ^= a; b -= rol32(a, 14);		\
	c ^= b; c -= rol32(b, 24);		\
}

/* An arbitrary initial parameter */
#define JHASH_INITVAL		0xdeadbeef

/**
 * jhash - hash an arbitrary key
 * @k: sequence of bytes as key
 * @length: the length of the key
 * @initval: the previous hash, or an arbitray value
 *
 * The generic version, hashes an arbitrary sequence of bytes.
 * No alignment or length assumptions are made about the input key.
 *
 * Returns the hash value of the key. The result depends on endianness.
 */
extern uint32_t jhash(const void *key, uint32_t length, uint32_t initval);

/* jhash2 - hash an array of uint32_t's
 * @k: the key which must be an array of uint32_t's
 * @length: the number of uint32_t's in the key
 * @initval: the previous hash, or an arbitray value
 *
 * Returns the hash value of the key.
 */
extern uint32_t jhash2(const uint32_t *k, uint32_t length, uint32_t initval);

/* __jhash_nwords - hash exactly 3, 2 or 1 word(s) */
static inline uint32_t __jhash_nwords(uint32_t a, uint32_t b, uint32_t c, uint32_t initval)
{
	a += initval;
	b += initval;
	c += initval;
	__jhash_final(a, b, c);
	return c;
}

static inline uint32_t jhash_3words(uint32_t a, uint32_t b, uint32_t c, uint32_t initval)
{
	return __jhash_nwords(a, b, c, initval + JHASH_INITVAL + (3 << 2));
}

static inline uint32_t jhash_2words(uint32_t a, uint32_t b, uint32_t initval)
{
	return __jhash_nwords(a, b, 0, initval + JHASH_INITVAL + (2 << 2));
}

static inline uint32_t jhash_1word(uint32_t a, uint32_t initval)
{
	return __jhash_nwords(a, 0, 0, initval + JHASH_INITVAL + (1 << 2));
}

#define GOLDEN_RATIO_32 0x61C88647
#define GOLDEN_RATIO_64 0x61C8864680B583EBull

#if BITS_PER_LONG == 32
# define GOLDEN_RATIO_PRIME GOLDEN_RATIO_32
# define __hash_long(val) __hash_32((val))
# define hash_long(val, bits) hash_32(val, bits)
#elif BITS_PER_LONG == 64
# define __hash_long(val) hash_64((val), 48)
# define hash_long(val, bits) hash_64(val, bits)
# define GOLDEN_RATIO_PRIME GOLDEN_RATIO_64
#else
# error Wordsize not 32 or 64
#endif

static inline uint32_t __hash_32(uint32_t val)
{
	return val * GOLDEN_RATIO_32;
}

static inline uint64_t __hash_64(uint64_t val)
{
#if BITS_PER_LONG == 64
	/* 64x64-bit multiply is efficient on all 64-bit processors */
	return (val * GOLDEN_RATIO_64);
#else
	/* Hash 64 bits using only 32x32-bit multiply. */
	return (((uint64_t)__hash_32((uint32_t)val)) << 32) | __hash_32(val >> 32);
#endif
}

static inline uint32_t hash_32(uint32_t val, unsigned int bits)
{
	/* High bits are more random, so use them. */
	return __hash_32(val) >> (32 - bits);
}

static __always_inline uint32_t hash_64(uint64_t val, unsigned int bits)
{
	return (uint32_t)(__hash_64(val) >> (64 - bits));
}

static inline uint32_t hash_ptr(const void *ptr, unsigned int bits)
{
	return hash_long((unsigned long)ptr, bits);
}

/* This really should be called fold32_ptr; it does no hashing to speak of. */
static inline uint32_t hash32_ptr(const void *ptr)
{
	unsigned long long val = (unsigned long long)ptr;
#if BITS_PER_LONG == 64
	val ^= (val >> 32);
#endif
	return (uint32_t)val;
}

typedef uint32_t (*hash_fn)(const void*, long);

extern uint32_t MurmurHash2(const void *key, ssize_t keyLen);
extern uint32_t HashFlower(const void *key, ssize_t keyLen);
extern uint32_t HashTime33(const void *key, ssize_t keyLen);
extern uint32_t HashIgnoreCaseTime33(const void *key, ssize_t keyLen);
extern uint32_t HashReduceBit(const void *key, ssize_t keyLen);
extern uint32_t HashPJW(const void *key, ssize_t keyLen);

/*计算检验和*/
extern uint32_t byteCrc32(const void *buf, size_t size, uint32_t init);

__END_DECLS

#endif
