#ifndef __US_BITMAP_H__
#define __US_BITMAP_H__

#include <string.h>
#include "bitops.h"

__BEGIN_DECLS

#define DECLARE_BITMAP(name,bits) \
	unsigned long name[BITS_TO_LONGS(bits)]

#define BITMAP_SIZE(bits) \
	(sizeof(unsigned long) * BITS_TO_LONGS(bits))

/*generate like 0xfffffffe*/
#define BITMAP_FIRST_WORD_MASK(start) (~0UL << ((start) & (BITS_PER_LONG - 1)))
/*generate like 0x7fffffff*/
#define BITMAP_LAST_WORD_MASK(nbits) (~ BITMAP_FIRST_WORD_MASK(nbits))

#ifdef __LITTLE_ENDIAN
# define BITMAP_MEM_ALIGNMENT 8
#else
# define BITMAP_MEM_ALIGNMENT (8 * sizeof(unsigned long))
#endif

#define BITMAP_MEM_MASK (BITMAP_MEM_ALIGNMENT - 1)

#define small_const_nbits(nbits) \
	(__builtin_constant_p(nbits) && (nbits) <= BITS_PER_LONG)

////////////////////////////////////////////////////////////////////////////////
// 位图初始化操作
////////////////////////////////////////////////////////////////////////////////
static inline void bitmap_zero(unsigned long *dst, unsigned int nbits)
{
	if (small_const_nbits(nbits))
		*dst = 0UL;
	else {
		unsigned int len = BITS_TO_LONGS(nbits) * sizeof(unsigned long);
		memset(dst, 0, len);
	}
}

static inline void bitmap_fill(unsigned long *dst, unsigned int nbits)
{
	if (small_const_nbits(nbits))
		*dst = ~0UL;
	else {
		unsigned int len = BITS_TO_LONGS(nbits) * sizeof(unsigned long);
		memset(dst, 0xff, len);
	}
}

static inline void bitmap_copy(unsigned long *dst, const unsigned long *src,
		unsigned int nbits)
{
	if (small_const_nbits(nbits))
		*dst = *src;
	else {
		unsigned int len = BITS_TO_LONGS(nbits) * sizeof(unsigned long);
		memcpy(dst, src, len);
	}
}

/*
 * Copy bitmap and clear tail bits in last word.
 */
static inline void bitmap_copy_clear_tail(unsigned long *dst,
		const unsigned long *src, unsigned int nbits)
{
	bitmap_copy(dst, src, nbits);
	if (nbits % BITS_PER_LONG)
		dst[nbits / BITS_PER_LONG] &= BITMAP_LAST_WORD_MASK(nbits);
}

////////////////////////////////////////////////////////////////////////////////
// 位图计算操作，交集、差集、合集等
////////////////////////////////////////////////////////////////////////////////

extern unsigned int __bitmap_weight(const unsigned long *bitmap,
		unsigned long nbits);

extern int __bitmap_equal(const unsigned long *bitmap1,
		const unsigned long *bitmap2, unsigned int nbits);

extern void __bitmap_complement(unsigned long *dst,
		const unsigned long *src, unsigned int nbits);

extern int __bitmap_and(unsigned long *dst, const unsigned long *bitmap1,
		const unsigned long *bitmap2, unsigned int nbits);
extern void __bitmap_or(unsigned long *dst, const unsigned long *bitmap1,
		const unsigned long *bitmap2, unsigned int nbits);
extern void __bitmap_xor(unsigned long *dst, const unsigned long *bitmap1,
		const unsigned long *bitmap2, unsigned int nbits);
extern int __bitmap_andnot(unsigned long *dst, const unsigned long *bitmap1,
		const unsigned long *bitmap2, unsigned int nbits);

extern int __bitmap_intersects(const unsigned long *bitmap1,
		const unsigned long *bitmap2, unsigned int nbits);
extern int __bitmap_subset(const unsigned long *bitmap1,
		const unsigned long *bitmap2, unsigned int nbits);

extern void __bitmap_set(unsigned long *map, unsigned int start, int len);
extern void __bitmap_clear(unsigned long *map, unsigned int start, int len);

extern void __bitmap_shift_right(unsigned long *dst, const unsigned long *src,
		unsigned int shift, unsigned int nbits);
extern void __bitmap_shift_left(unsigned long *dst, const unsigned long *src,
		unsigned int shift, unsigned int nbits);

////////////////////////////////////////////////////////////////////////////////
/**返回位图中被置位的位数*/
static __always_inline int bitmap_weight(const unsigned long *src,
		unsigned int nbits)
{
	if (small_const_nbits(nbits))
		return hweight_long(*src & BITMAP_LAST_WORD_MASK(nbits));
	return __bitmap_weight(src, nbits);
}

/**两个位图是否相等*/
static __always_inline int bitmap_equal(const unsigned long *src1,
		const unsigned long *src2, unsigned int nbits)
{
	if (small_const_nbits(nbits))
		return !((*src1 ^ *src2) & BITMAP_LAST_WORD_MASK(nbits));
	if (__builtin_constant_p(nbits & BITMAP_MEM_MASK) &&
		IS_ALIGNED(nbits, BITMAP_MEM_ALIGNMENT))
		return !memcmp(src1, src2, nbits / 8);
	return __bitmap_equal(src1, src2, nbits);
}

/**将源位图 src 的数据取反存储在目标位图 dst 中*/
static __always_inline void bitmap_complement(unsigned long *dst,
		const unsigned long *src, unsigned int nbits)
{
	if (small_const_nbits(nbits))
		*dst = ~(*src);
	else
		__bitmap_complement(dst, src, nbits);
}

/**
 * 将源位图 src1 的数据 与上 源位图 src2 的数据存储在目标位图 dst 中，
 * 即求交集，如果存在交集则返回真
 */
static __always_inline int bitmap_and(unsigned long *dst,
		const unsigned long *src1, const unsigned long *src2,
		unsigned int nbits)
{
	if (small_const_nbits(nbits))
		return (*dst = *src1 & *src2 & BITMAP_LAST_WORD_MASK(nbits)) != 0;
	return __bitmap_and(dst, src1, src2, nbits);
}

/**将源位图 src1 的数据 或上 源位图 src2 的数据存储在目标位图 dst 中，即求和集*/
static __always_inline void bitmap_or(unsigned long *dst,
		const unsigned long *src1, const unsigned long *src2,
		unsigned int nbits)
{
	if (small_const_nbits(nbits))
		*dst = *src1 | *src2;
	else
		__bitmap_or(dst, src1, src2, nbits);
}

/**将源位图 src1 的数据 异或上 源位图 src2 的数据存储在目标位图 dst 中，即求对称差集*/
static __always_inline void bitmap_xor(unsigned long *dst,
		const unsigned long *src1, const unsigned long *src2,
		unsigned int nbits)
{
	if (small_const_nbits(nbits))
		*dst = *src1 ^ *src2;
	else
		__bitmap_xor(dst, src1, src2, nbits);
}

/**将源位图 src1 的数据 与上 源位图 src2 的数据反集 存储在目标位图 dst 中，即求补集*/
static __always_inline int bitmap_andnot(unsigned long *dst,
		const unsigned long *src1, const unsigned long *src2,
		unsigned int nbits)
{
	if (small_const_nbits(nbits))
		return (*dst = *src1 & ~(*src2) & BITMAP_LAST_WORD_MASK(nbits)) != 0;
	return __bitmap_andnot(dst, src1, src2, nbits);
}

/**是否存在差集*/
static inline int bitmap_intersects(const unsigned long *src1,
		const unsigned long *src2, unsigned int nbits)
{
	if (small_const_nbits(nbits))
		return ((*src1 & *src2) & BITMAP_LAST_WORD_MASK(nbits)) != 0;
	else
		return __bitmap_intersects(src1, src2, nbits);
}

/**是否为子集*/
static inline int bitmap_subset(const unsigned long *src1,
		const unsigned long *src2, unsigned int nbits)
{
	if (small_const_nbits(nbits))
		return ! ((*src1 & ~(*src2)) & BITMAP_LAST_WORD_MASK(nbits));
	else
		return __bitmap_subset(src1, src2, nbits);
}

static __always_inline void bitmap_set(unsigned long *map, unsigned int start,
		unsigned int nbits)
{
	if (__builtin_constant_p(nbits) && nbits == 1)
		__set_bit(start, map);
	else if (__builtin_constant_p(start & BITMAP_MEM_MASK) &&
			 IS_ALIGNED(start, BITMAP_MEM_ALIGNMENT) &&
			 __builtin_constant_p(nbits & BITMAP_MEM_MASK) &&
			 IS_ALIGNED(nbits, BITMAP_MEM_ALIGNMENT))
		memset((char *)map + start / 8, 0xff, nbits / 8);
	else
		__bitmap_set(map, start, nbits);
}

static __always_inline void bitmap_clear(unsigned long *map, unsigned int start,
		unsigned int nbits)
{
	if (__builtin_constant_p(nbits) && nbits == 1)
		__clear_bit(start, map);
	else if (__builtin_constant_p(start & BITMAP_MEM_MASK) &&
			 IS_ALIGNED(start, BITMAP_MEM_ALIGNMENT) &&
			 __builtin_constant_p(nbits & BITMAP_MEM_MASK) &&
			 IS_ALIGNED(nbits, BITMAP_MEM_ALIGNMENT))
		memset((char *)map + start / 8, 0, nbits / 8);
	else
		__bitmap_clear(map, start, nbits);
}

static inline void bitmap_shift_right(unsigned long *dst,
		const unsigned long *src, unsigned int shift, int nbits)
{
	if (small_const_nbits(nbits))
		*dst = (*src & BITMAP_LAST_WORD_MASK(nbits)) >> shift;
	else
		__bitmap_shift_right(dst, src, shift, nbits);
}

static inline void bitmap_shift_left(unsigned long *dst,
		const unsigned long *src, unsigned int shift, unsigned int nbits)
{
	if (small_const_nbits(nbits))
		*dst = (*src << shift) & BITMAP_LAST_WORD_MASK(nbits);
	else
		__bitmap_shift_left(dst, src, shift, nbits);
}

////////////////////////////////////////////////////////////////////////////////
// 位图遍历操作
////////////////////////////////////////////////////////////////////////////////
/**
 * find_first_bit - find the first set bit in a memory region
 * @addr: The address to start the search at
 * @nbits: The maximum number of bits to search
 *
 * Returns the bit number of the first set bit.
 * If no bits are set, returns @size.
 */
extern unsigned long _find_first_bit(const unsigned long *addr,
	unsigned long nbits);

/**
 * find_last_bit - find the last set bit in a memory region
 * @addr: The address to start the search at
 * @size: The number of bits to search
 *
 * Returns the bit number of the last set bit, or size.
 */
extern unsigned long find_last_bit(const unsigned long *addr,
	unsigned long nbits);

/**
 * This is a common helper function for find_next_bit, find_next_zero_bit, and
 * find_next_and_bit. The differences are:
 *  - The "invert" argument, which is XORed with each fetched word before
 *    searching it for one bits.
 *  - The optional "addr2", which is anded with "addr1" if present.
 */
extern unsigned long __find_next_bit(const unsigned long *addr1,
	const unsigned long *addr2, unsigned long nbits,
	unsigned long start, unsigned long invert);

/**
 * find_next_bit - find the next set bit in a memory region
 * @addr: The address to base the search on
 * @offset: The bitnumber to start searching at
 * @nbits: The bitmap size in bits
 *
 * Returns the bit number for the next set bit
 * If no bits are set, returns @size.
 */
static inline unsigned long _find_next_bit(const unsigned long *addr,
	unsigned long nbits, unsigned long offset)
{
	return __find_next_bit(addr, NULL, nbits, offset, 0UL);
}

/**
 * find_next_and_bit - find the next set bit in both memory regions
 * @addr1: The first address to base the search on
 * @addr2: The second address to base the search on
 * @offset: The bitnumber to start searching at
 * @nbits: The bitmap size in bits
 *
 * Returns the bit number for the next set bit
 * If no bits are set, returns @size.
 */
static inline unsigned long _find_next_and_bit(const unsigned long *addr1,
	const unsigned long *addr2, unsigned long nbits, unsigned long offset)
{
	return __find_next_bit(addr1, addr2, nbits, offset, 0UL);
}

/**
 * find_next_zero_bit - find the next cleared bit in a memory region
 * @addr: The address to base the search on
 * @offset: The bitnumber to start searching at
 * @nbits: The bitmap size in bits
 *
 * Returns the bit number of the next zero bit
 * If no bits are zero, returns @size.
 */
static inline unsigned long _find_next_zero_bit(const unsigned long *addr,
	unsigned long nbits, unsigned long offset)
{
	return __find_next_bit(addr, NULL, nbits, offset, ~0UL);
}

/**
 * find_first_zero_bit - find the first cleared bit in a memory region
 * @addr: The address to start the search at
 * @nbits: The maximum number of bits to search
 *
 * Returns the bit number of the first cleared bit.
 * If no bits are zero, returns @size.
 */
extern unsigned long _find_first_zero_bit(const unsigned long *addr,
	unsigned long nbits);


/* return index of first bet set in val or max when no bit is set */
static inline unsigned long __scanbit(unsigned long val, unsigned long max)
{
	//asm("bsfq %1,%0 ; cmovz %2,%0" : "=&r" (val) : "r" (val), "r" (max));
	return (!val) ? max : __ffsl(val);
}

////////////////////////////////////////////////////////////////////////////////

#define find_first_bit(addr,nbits)											\
	((__builtin_constant_p(nbits) && (nbits) <= BITS_PER_LONG ?				\
	(__scanbit(*(unsigned long *)addr,(nbits))) :							\
		_find_first_bit(addr,nbits)))

#define find_next_bit(addr,nbits,off)										\
	((__builtin_constant_p(nbits) && (nbits) <= BITS_PER_LONG ? 	  		\
	((off) + (__scanbit((*(unsigned long *)addr) >> (off),(nbits)-(off)))) :\
		_find_next_bit(addr,nbits,off)))

#define find_first_zero_bit(addr,nbits)										\
	((__builtin_constant_p(nbits) && (nbits) <= BITS_PER_LONG ?				\
	(__scanbit(~*(unsigned long *)addr,(nbits))) :							\
		_find_first_zero_bit(addr,nbits)))

#define find_next_zero_bit(addr,nbits,off)									\
	((__builtin_constant_p(nbits) && (nbits) <= BITS_PER_LONG ? 	  		\
	((off)+(__scanbit(~(((*(unsigned long *)addr)) >> (off)),(nbits)-(off))	\
		)) : _find_next_zero_bit(addr,nbits,off)))

#define for_each_set_bit(bit, addr, nbits)									\
	for ((bit) = find_first_bit((addr), (nbits));							\
	     (bit) < (nbits);													\
	     (bit) = find_next_bit((addr), (nbits), (bit) + 1))

/* same as for_each_set_bit() but use bit as value to start with */
#define for_each_set_bit_from(bit, addr, nbits)								\
	for ((bit) = find_next_bit((addr), (nbits), (bit));						\
	     (bit) < (nbits);													\
	     (bit) = find_next_bit((addr), (nbits), (bit) + 1))

#define for_each_clear_bit(bit, addr, nbits)								\
	for ((bit) = find_first_zero_bit((addr), (nbits));						\
	     (bit) < (nbits);													\
	     (bit) = find_next_zero_bit((addr), (nbits), (bit) + 1))

/* same as for_each_clear_bit() but use bit as value to start with */
#define for_each_clear_bit_from(bit, addr, nbits)							\
	for ((bit) = find_next_zero_bit((addr), (nbits), (bit));				\
	     (bit) < (nbits);													\
	     (bit) = find_next_zero_bit((addr), (nbits), (bit) + 1))


static inline bool bitmap_empty(const unsigned long *src, unsigned nbits)
{
	if (small_const_nbits(nbits))
		return ! (*src & BITMAP_LAST_WORD_MASK(nbits));

	return find_first_bit(src, nbits) == nbits;
}

static inline bool bitmap_full(const unsigned long *src, unsigned int nbits)
{
	if (small_const_nbits(nbits))
		return ! (~(*src) & BITMAP_LAST_WORD_MASK(nbits));

	return find_first_zero_bit(src, nbits) == nbits;
}

#if BITS_PER_LONG == 64
# define BITMAP_FROM_U64(n) (n)
#else
# define BITMAP_FROM_U64(n) ((unsigned long) ((u64)(n) & ULONG_MAX)),		\
				((unsigned long) ((u64)(n) >> 32))
#endif

static inline void bitmap_from_u64(unsigned long *dst, uint64_t mask)
{
	dst[0] = mask & ULONG_MAX;

	if (sizeof(mask) > sizeof(unsigned long))
		dst[1] = mask >> 32;
}

#if BITS_PER_LONG == 64
extern void bitmap_from_arr32(unsigned long *bitmap, const uint32_t *buf,
		unsigned int nbits);
extern void bitmap_to_arr32(uint32_t *buf, const unsigned long *bitmap,
		unsigned int nbits);
#else
static inline void bitmap_from_arr32(unsigned long *bitmap, const uint32_t *buf,
		unsigned int nbits)
{
	if (nbits)
		bitmap_copy_clear_tail((unsigned long *) (bitmap),
			(const unsigned long *) (buf), (nbits));
}
static inline void bitmap_to_arr32(uint32_t *buf, const unsigned long *bitmap,
		unsigned int nbits)
{
	if (nbits)
		bitmap_copy_clear_tail((unsigned long *) (buf),
			(const unsigned long *) (bitmap), (nbits));
}
#endif

/**
 * bitmap_parselist - convert list format ASCII string to bitmap
 * @buf: read nul-terminated user string from this buffer
 * @buflen: buffer size in bytes.  If string is smaller than this
 *    then it must be terminated with a \0.
 * @bitmap: write resulting mask here
 * @nbits: number of bits in mask to be written
 *
 * Input format is a comma-separated list of decimal numbers and
 * ranges.  Consecutively set bits are shown as two hyphen-separated
 * decimal numbers, the smallest and largest bit numbers set in
 * the range.
 * Optionally each range can be postfixed to denote that only parts of it
 * should be set. The range will divided to groups of specific size.
 * From each group will be used only defined amount of bits.
 * Syntax: range:used_size/group_size
 * Example: 0-1023:2/256 ==> 0,1,256,257,512,513,768,769
 *
 * Returns: 0 on success, -errno on invalid input strings. Error values:
 *
 *   - ``-EINVAL``: second number in range smaller than first
 *   - ``-EINVAL``: invalid character in string
 *   - ``-ERANGE``: bit number specified too large for mask
 */
extern int __bitmap_parselist(const char *buf, size_t buflen,
		unsigned long *bitmap, unsigned int nbits);

extern int bitmap_parselist(const char *bp, unsigned long *bitmap,
		unsigned int nbits);

__END_DECLS

#endif
