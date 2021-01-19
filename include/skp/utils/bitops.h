/*
 * @Author: kai.zhou
 * @Date: 2018-09-11 11:01:33
 */
#ifndef __SU_BITOPS_H__
#define __SU_BITOPS_H__

#include "compiler.h"

__BEGIN_DECLS

#define BIT_64(n)			((1ULL) << (n))
#define BIT(nr)				(1UL << (nr))
#define BIT_ULL(nr)			(1ULL << (nr))
#define BIT_OFFSET(nr)		((nr) % BITS_PER_LONG)
#define BIT_MASK(nr)		(1UL << ((nr) % BITS_PER_LONG))
#define BIT_WORD(nr)		((nr) / BITS_PER_LONG)
#define BIT_ULL_MASK(nr)	(1ULL << ((nr) % BITS_PER_LONG_LONG))
#define BIT_ULL_WORD(nr)	((nr) / BITS_PER_LONG_LONG)
#define BITS_PER_BYTE		8
#define BITS_TO_LONGS(nr)	(((nr) + (BITS_PER_BYTE * sizeof(long)) - 1) /	\
							(BITS_PER_BYTE * sizeof(long)))

////////////////////////////////////////////////////////////////////////////////

#define __ALIGN_MASK(x, mask)	(((x) + (mask)) & ~(mask))
#define __ALIGN(x, a)			__ALIGN_MASK(x, (typeof(x))(a) - 1)

/* @a is a power of 2 value */
#define ALIGN(x, a)				__ALIGN((x), (a))
#define ALIGN_DOWN(x, a)		__ALIGN((x) - ((a) - 1), (a))
#define PTR_ALIGN(p, a)			((typeof(p))ALIGN((unsigned long)(p), (a)))
#define IS_ALIGNED(x, a)		(((x) & ((typeof(x))(a) - 1)) == 0)

/*向上求倍数*/
#define DIV_ROUND_UP(n, d)		(((n) + (d) - 1) / (d))

/*
 * Create a contiguous bitmask starting at bit position @l and ending at
 * position @h. For example
 * GENMASK_ULL(39, 21) gives us the 64bit vector 0x000000ffffe00000.
 */
#define GENMASK(h, l)														\
	(((~0UL) - (1UL << (l)) + 1) & (~0UL >> (BITS_PER_LONG - 1 - (h))))

#define GENMASK_ULL(h, l)													\
	(((~0ULL) - (1ULL << (l)) + 1) & (~0ULL >> (BITS_PER_LONG_LONG - 1 - (h))))

/*ensure that data of variable been read from memory*/
extern void __read_once_size__(const volatile void *, void*, size_t);
extern void __write_once_size__(volatile void *, void*, size_t);

static __always_inline void __read_once_size(const volatile void *p, void *res,
		size_t size)
{
	switch (size) {
		case 1: *(uint8_t*)res = *(volatile uint8_t*)p; break;
		case 2: *(uint16_t*)res = *(volatile uint16_t*)p; break;
		case 4: *(uint32_t*)res = *(volatile uint32_t*)p; break;
#ifdef __x86_64__
		case 8: *(uint64_t*)res = *(volatile uint64_t*)p; break;
#endif
		default:
			__read_once_size__(p, res, size);
	}
	static_mb();
}

/*ensure that data of variable been write to memory*/
static __always_inline void __write_once_size(volatile void *p, void *res,
		size_t size)
{
	switch (size) {
		case 1: *(volatile uint8_t*)p = *(uint8_t *)res; break;
		case 2: *(volatile uint16_t *)p = *(uint16_t *)res; break;
		case 4: *(volatile uint32_t *)p = *(uint32_t *)res; break;
#ifdef __x86_64__
		case 8: *(volatile uint64_t *)p = *(uint64_t *)res; break;
#endif
		default:
			__write_once_size__(p, res, size);
	}
	static_mb();
}

#define READ_ONCE(x)														\
({																			\
	union { typeof(x) __val; char __c[1]; } __u =							\
		{ .__val = (typeof(x)) (0) }; 										\
	__read_once_size(&(x), __u.__c, sizeof(x));								\
	__u.__val;																\
})

#define WRITE_ONCE(x, val) 													\
({																			\
	union { typeof(x) __val; char __c[1]; } __u =							\
		{ .__val = (typeof(x)) (val) }; 									\
	__write_once_size(&(x), __u.__c, sizeof(x));							\
	__u.__val;																\
})

////////////////////////////////////////////////////////////////////////////////
// 位的旋转
////////////////////////////////////////////////////////////////////////////////
/**
 * 将左边的 shift 位 旋转到右边
 *           (2)
 * 1011 1110 --> 1111 1010
 * 或
 * 将右边的 shift 位 旋转到左边
 *           (2)
 * 1110 1110 --> 1011 1011
 */
#define __defunc_rolr__(bits)												\
static inline uint##bits##_t rol##bits(uint##bits##_t word, unsigned shift)	\
{ return (word << shift) | (word >> (bits - shift)); }						\
static inline uint##bits##_t ror##bits(uint##bits##_t word, unsigned shift)	\
{ return (word >> shift) | (word << (bits - shift)); }

__defunc_rolr__(8)
__defunc_rolr__(16)
__defunc_rolr__(32)
__defunc_rolr__(64)

/*
 * 交换双字节 
 * 0x1234 => 0x3412
 */
#if !defined(SWAP_2BYTES)
  #define SWAP_2BYTES(x)													\
	({ short __v = (short)(x);												\
		(((__v & 0xff00U) >> 8) | ((__v & 0x00ffU) << 8)); })
#endif

/*
 * 交换四字节
 * 0x12345678 => 0x78563412
 */
#if !defined(SWAP_4BYTES)
  #define SWAP_4BYTES(x)													\
	({ int __v = (int)(x); 													\
		(((__v & 0xff000000U) >> 24) | ((__v & 0x00ff0000U) >> 8) |			\
		((__v & 0x0000ff00U) << 8) | ((__v & 0x000000ffU) << 24)); })
#endif

/*
 * 交换八字节
 * 0x1234567890abcdef => 0xefcdab9078563412
 */
#if !defined(SWAP_8BYTES)
  #define SWAP_8BYTES(x)													\
	({ long long __v = (long long)(x);										\
		(((__v & 0xff00000000000000ULL) >> 56) |							\
		((__v & 0x00ff000000000000) >> 40) |  								\
		((__v & 0x0000ff0000000000ULL) >> 24) |								\
		((__v & 0x000000ff00000000) >> 8) |									\
		((__v & 0x00000000ff000000ULL) << 8) |								\
		((__v & 0x0000000000ff0000) << 24) |								\
		((__v & 0x000000000000ff00ULL) << 40) |								\
		((__v & 0x00000000000000ff) << 56)); })
#endif

/*64位 主机与网络字节序 转换*/
#include <arpa/inet.h>

#ifndef htonll
static inline long long htonll(long long v)
{
# ifdef __BIG_ENDIAN
	return v;
# else
	return SWAP_8BYTES(v);
# endif
}
#endif

#ifndef htonll
static inline long long ntohll(long long v)
{
# ifdef __BIG_ENDIAN
	return v;
# else
	return SWAP_8BYTES(v);
# endif
}
#endif

////////////////////////////////////////////////////////////////////////////////
// 位测试与修改
////////////////////////////////////////////////////////////////////////////////
#define __ASM_FORM(x)	" " #x " "
#define __ASM_FORM_RAW(x)     #x
#define __ASM_FORM_COMMA(x) " " #x ","

#if BITS_PER_LONG == 32
/* 32 bit */
# define __ASM_SEL(a,b) __ASM_FORM(a)
# define __ASM_SEL_RAW(a,b) __ASM_FORM_RAW(a)
#else
/* 64 bit */
# define __ASM_SEL(a,b) __ASM_FORM(b)
# define __ASM_SEL_RAW(a,b) __ASM_FORM_RAW(b)
#endif

#define __ASM_SIZE(inst, ...) __ASM_SEL(inst##l##__VA_ARGS__,				\
										inst##q##__VA_ARGS__)
#define __ASM_REG(reg) __ASM_SEL_RAW(e##reg, r##reg)

/*
 * Macros to generate condition code outputs from inline assembly,
 * The output operand must be type "bool".
 */
#define CC_SET(c) "\n\tset" #c " %[_cc_" #c "]\n"
#define CC_OUT(c) [_cc_ ## c] "=qm"

#define LOCK_PREFIX "\n\tlock; "

#define __CLOBBERS_MEM(clb...)	"memory", ## clb

/* Use flags output or a set instruction */
#define __GEN_RMWcc(fullop, var, cc, clobbers, ...)							\
do {																		\
	bool c;																	\
	asm volatile (fullop CC_SET(cc)											\
			: [counter] "+m" (var), CC_OUT(cc) (c)							\
			: __VA_ARGS__ : clobbers);										\
	return c;																\
} while (0)

#define __BINARY_RMWcc_ARG	" %2, "

#define GEN_UNARY_RMWcc(op, var, arg0, cc)									\
	__GEN_RMWcc(op " " arg0, var, cc, __CLOBBERS_MEM())

#define GEN_UNARY_SUFFIXED_RMWcc(op, suffix, var, arg0, cc, clobbers...)	\
	__GEN_RMWcc(op " " arg0 "\n\t" suffix, var, cc, __CLOBBERS_MEM(clobbers))

#define GEN_BINARY_RMWcc(op, var, vcon, val, arg0, cc)						\
	__GEN_RMWcc(op __BINARY_RMWcc_ARG arg0, var, cc, __CLOBBERS_MEM(),		\
		vcon (val))

#define GEN_BINARY_SUFFIXED_RMWcc(op,suffix,v,vcon,val,arg0,cc,clobbers...)	\
	__GEN_RMWcc(op __BINARY_RMWcc_ARG arg0 "\n\t" suffix,					\
		v, cc, __CLOBBERS_MEM(clobbers), vcon (val))

#if __GNUC__ < 4 || (__GNUC__ == 4 && __GNUC_MINOR__ < 1)
/*Technically wrong, but this avoids compilation errors on some gcc versions.*/
# define BITOP_ADDR(x) "=m" (*(volatile long *) (x))
#else
# define BITOP_ADDR(x) "+m" (*(volatile long *) (x))
#endif

#define IS_IMMEDIATE(nr) (__builtin_constant_p(nr))
#define CONST_MASK(nr) (1 << ((nr) & 7))
#define CONST_MASK_ADDR(nr, addr)	BITOP_ADDR((char *)(addr) + ((nr)>>3))

static inline void set_bit(long nr, volatile unsigned long *addr)
{
	if (IS_IMMEDIATE(nr)) {
		asm volatile(LOCK_PREFIX "orb %1,%0"
			: CONST_MASK_ADDR(nr, addr)
			: "iq" ((uint8_t)CONST_MASK(nr))
			: "memory");
	} else {
		asm volatile(LOCK_PREFIX __ASM_SIZE(bts) " %1,%0"
			: BITOP_ADDR(addr) : "Ir" (nr) : "memory");
	}
}

static inline void clear_bit(long nr, volatile unsigned long *addr)
{
	if (IS_IMMEDIATE(nr)) {
		asm volatile(LOCK_PREFIX "andb %1,%0"
			: CONST_MASK_ADDR(nr, addr)
			: "iq" ((uint8_t)~CONST_MASK(nr)));
	} else {
		asm volatile(LOCK_PREFIX __ASM_SIZE(btr) " %1,%0"
			: BITOP_ADDR(addr)
			: "Ir" (nr));
	}
}

static inline void change_bit(long nr, volatile unsigned long *addr)
{
	if (IS_IMMEDIATE(nr)) {
		asm volatile(LOCK_PREFIX "xorb %1,%0"
			: CONST_MASK_ADDR(nr, addr)
			: "iq" ((uint8_t)CONST_MASK(nr)));
	} else {
		asm volatile(LOCK_PREFIX __ASM_SIZE(btc) " %1,%0"
			: BITOP_ADDR(addr)
			: "Ir" (nr));
	}
}

static inline bool test_and_set_bit(long nr, volatile unsigned long *addr)
{
	GEN_BINARY_RMWcc(LOCK_PREFIX __ASM_SIZE(bts), *addr, "Ir", nr, "%0", c);
}

static inline bool test_and_clear_bit(long nr, volatile unsigned long *addr)
{
	GEN_BINARY_RMWcc(LOCK_PREFIX __ASM_SIZE(btr), *addr, "Ir", nr, "%0", c);
}

static inline bool test_and_change_bit(long nr, volatile unsigned long *addr)
{
	GEN_BINARY_RMWcc(LOCK_PREFIX __ASM_SIZE(btc), *addr, "Ir", nr, "%0", c);
}

static inline void __set_bit(long nr, volatile unsigned long *addr)
{
	asm volatile(__ASM_SIZE(bts) " %1,%0" : BITOP_ADDR(addr) : "Ir" (nr)
		: "memory");
}

static inline void __clear_bit(long nr, volatile unsigned long *addr)
{
	asm volatile(__ASM_SIZE(btr) " %1,%0" : BITOP_ADDR(addr) : "Ir" (nr));
}

static inline void __change_bit(long nr, volatile unsigned long *addr)
{
	asm volatile(__ASM_SIZE(btc) " %1,%0" : BITOP_ADDR(addr) : "Ir" (nr));
}

static inline bool __test_and_set_bit(long nr, volatile unsigned long *addr)
{
	bool oldbit;
	asm volatile(__ASM_SIZE(bts) " %2,%1" CC_SET(c)
		: CC_OUT(c) (oldbit), BITOP_ADDR(addr)
		: "Ir" (nr));
	return oldbit;
}

static inline bool __test_and_clear_bit(long nr, volatile unsigned long *addr)
{
	bool oldbit;
	asm volatile(__ASM_SIZE(btr) " %2,%1" CC_SET(c)
		: CC_OUT(c) (oldbit), BITOP_ADDR(addr)
		: "Ir" (nr));
	return oldbit;
}

static inline bool __test_and_change_bit(long nr, volatile unsigned long *addr)
{
	bool oldbit;
	asm volatile(__ASM_SIZE(btc) " %2,%1" CC_SET(c)
		: CC_OUT(c) (oldbit), BITOP_ADDR(addr)
		: "Ir" (nr)
		: "memory");
	return oldbit;
}

static inline bool constant_test_bit(long nr,const volatile unsigned long *addr)
{
	return ((1UL << (nr & (BITS_PER_LONG-1))) &
		(READ_ONCE(addr[nr >> BITS_PER_LONG_SHIFT]))) != 0;
}

static inline bool variable_test_bit(long nr, const volatile unsigned long *addr)
{
	bool oldbit;
	asm volatile(__ASM_SIZE(bt) " %2,%1" CC_SET(c)
		: CC_OUT(c) (oldbit)
		: "m" (*(unsigned long *)addr), "Ir" (nr));
	return oldbit;
}

#define test_bit(nr, addr)													\
	(__builtin_constant_p((nr)) ? 											\
		constant_test_bit((nr), (const volatile unsigned long*)(addr))		\
		: variable_test_bit((nr), (const volatile unsigned long*)(addr)))

static inline void clear_bit_unlock(long nr, volatile unsigned long *addr)
{
	static_mb();
	clear_bit(nr, addr);
}

static inline void __clear_bit_unlock(long nr, volatile unsigned long *addr)
{
	static_mb();
	__clear_bit(nr, addr);
}

static inline bool test_and_set_bit_lock(long nr, volatile unsigned long *addr)
{
	return test_and_set_bit(nr, addr);
}

////////////////////////////////////////////////////////////////////////////////
// 位计数与定位 
////////////////////////////////////////////////////////////////////////////////
/**
 * 找到最后一位被设置的位下标，下标从 1 开始
 * 如果值 x 为 0 ，则返回 0
 * __builtin_clz() 表示 count - leading - zero 即从高位起，连续设置为0的位的数量
 */
#include <string.h>
#include <strings.h>
#ifdef __linux__
static inline int fls(int x)
{
	return x ? sizeof(x) * 8 - __builtin_clz(x) : 0;
}
static inline int flsl(long x)
{
	return x ? sizeof(x) * 8 - __builtin_clzl(x) : 0;
}
static inline int flsll(long long x)
{
	return x ? sizeof(x) * 8 - __builtin_clzll(x) : 0;
}

//static inline int ffs(int x) { return __builtin_ffs(x); }
#if !defined(_GNU_SOURCE) && !defined(__USE_MISC)
static inline int ffsl(long x) { return __builtin_ffsl(x); }
static inline int ffsll(long long x) { return __builtin_ffsll(x); }
#endif
#endif

/**
 * 找到最后一位被设置的位下标，下标从 0 开始
 * 如果值 x 为 0 ，则结果未定义
 */
static inline int __fls(int x)
{
	return (sizeof(x) * 8) - 1 - __builtin_clz(x);
}
static inline int __flsl(long x)
{
	return (sizeof(x) * 8) - 1 - __builtin_clzl(x);
}
static inline int __flsll(long long x)
{
	return (sizeof(x) * 8) - 1 - __builtin_clzll(x);
}

/**
 * 找到第一位被设置的位下标，下标从 0 开始
 * 如果值 x 为 0 ，则结果未定义
 * __builtin_clz() 表示 count - trailing - zero 即从低位起，连续设置为0的位的数量
 */
static inline int __ffs(int x)
{
	return __builtin_ctz(x);
}
static inline int __ffsl(long x)
{
	return __builtin_ctzl(x);
}
static inline int __ffsll(long long x)
{
	return __builtin_ctzll(x);
}

/*已置位数*/
static inline int hweight32(uint32_t w)
{
	return __builtin_popcount(w);
}

static inline int hweight64(uint64_t w)
{
	return __builtin_popcountll(w);
}

#define hweight8(x) hweight32((uint8_t)(x))
#define hweight16(x) hweight32((uint16_t)(x))

static inline unsigned int hweight_long(unsigned long w)
{
	return sizeof(w) == 4 ? hweight32(w) : hweight64(w);
}

#define fls64(x) flsll((long long)(x))
#define ffs64(x) ffsll((long long)(x))
#define __fls64(x) __flsll((long long)(x))
#define __ffs64(x) __ffsll((long long)(x))

#define ffz(x)  __ffs(~(x))
#define ffzl(x)  __ffsl(~(x))
#define ffzll(x)  __ffsll(~(x))

////////////////////////////////////////////////////////////////////////////////
// 2 ^ n 指数计算
////////////////////////////////////////////////////////////////////////////////
/*获取 v 向上对齐到 2^n 的 指数*/
static inline int get_count_order(unsigned int v)
{
	if (v == 0U)
		return -1;
	else if (v & (v - 1U))
		return fls(v); /*非2^n，则向上对齐*/
	else
		return fls(v) - 1;
}

static inline int get_count_order_long(unsigned long v)
{
	if (v == 0UL)
		return -1;
	else if (v & (v - 1UL))
		return flsl(v); /*非2^n，则向上对齐*/
	else
		return flsl(v) - 1;
}

/*获取 v 向下 2^n 对齐的 指数*/
static inline int __ilog2_u32(uint32_t v)
{
	return fls(v) - 1;
}

static inline int __ilog2_u64(uint64_t v)
{
	return (int)(fls64(v) - 1);
}

/**
 * is_power_of_2() - check if a value is a power of two
 * @n: the value to check
 *
 * Determine whether some value is a power of two, where zero is
 * *not* considered a power of two.
 * Return: true if @n is a power of 2, otherwise false.
 */
static inline bool is_power_of_2(unsigned long n)
{
	return (n != 0 && ((n & (n - 1)) == 0));
}

/**
 * __roundup_pow_of_two() - round up to nearest power of two
 * @n: value to round up
 */
static inline unsigned long __roundup_pow_of_two(unsigned long n)
{
	return 1UL << flsl(n - 1);
}

/**
 * __rounddown_pow_of_two() - round down to nearest power of two
 * @n: value to round down
 */
static inline unsigned long __rounddown_pow_of_two(unsigned long n)
{
	return 1UL << (flsl(n) - 1);
}

/**
 * ilog2 - log base 2 of 32-bit or a 64-bit unsigned value
 * @n: parameter
 *
 * constant-capable log of base 2 calculation
 * - this can be used to initialise global variables from constant data, hence
 * the massive ternary operator construction
 *
 * selects the appropriately-sized optimised version depending on sizeof(n)
 */
#define ilog2_const(n) 														\
	((n) < 2UL ? 0 :														\
	(n) & (1ULL << 63) ? 63 :												\
	(n) & (1ULL << 62) ? 62 :												\
	(n) & (1ULL << 61) ? 61 :												\
	(n) & (1ULL << 60) ? 60 :												\
	(n) & (1ULL << 59) ? 59 :												\
	(n) & (1ULL << 58) ? 58 :												\
	(n) & (1ULL << 57) ? 57 :												\
	(n) & (1ULL << 56) ? 56 :												\
	(n) & (1ULL << 55) ? 55 :												\
	(n) & (1ULL << 54) ? 54 :												\
	(n) & (1ULL << 53) ? 53 :												\
	(n) & (1ULL << 52) ? 52 :												\
	(n) & (1ULL << 51) ? 51 :												\
	(n) & (1ULL << 50) ? 50 :												\
	(n) & (1ULL << 49) ? 49 :												\
	(n) & (1ULL << 48) ? 48 :												\
	(n) & (1ULL << 47) ? 47 :												\
	(n) & (1ULL << 46) ? 46 :												\
	(n) & (1ULL << 45) ? 45 :												\
	(n) & (1ULL << 44) ? 44 :												\
	(n) & (1ULL << 43) ? 43 :												\
	(n) & (1ULL << 42) ? 42 :												\
	(n) & (1ULL << 41) ? 41 :												\
	(n) & (1ULL << 40) ? 40 :												\
	(n) & (1ULL << 39) ? 39 :												\
	(n) & (1ULL << 38) ? 38 :												\
	(n) & (1ULL << 37) ? 37 :												\
	(n) & (1ULL << 36) ? 36 :												\
	(n) & (1ULL << 35) ? 35 :												\
	(n) & (1ULL << 34) ? 34 :												\
	(n) & (1ULL << 33) ? 33 :												\
	(n) & (1ULL << 32) ? 32 :												\
	(n) & (1ULL << 31) ? 31 :												\
	(n) & (1ULL << 30) ? 30 :												\
	(n) & (1ULL << 29) ? 29 :												\
	(n) & (1ULL << 28) ? 28 :												\
	(n) & (1ULL << 27) ? 27 :												\
	(n) & (1ULL << 26) ? 26 :												\
	(n) & (1ULL << 25) ? 25 :												\
	(n) & (1ULL << 24) ? 24 :												\
	(n) & (1ULL << 23) ? 23 :												\
	(n) & (1ULL << 22) ? 22 :												\
	(n) & (1ULL << 21) ? 21 :												\
	(n) & (1ULL << 20) ? 20 :												\
	(n) & (1ULL << 19) ? 19 :												\
	(n) & (1ULL << 18) ? 18 :												\
	(n) & (1ULL << 17) ? 17 :												\
	(n) & (1ULL << 16) ? 16 :												\
	(n) & (1ULL << 15) ? 15 :												\
	(n) & (1ULL << 14) ? 14 :												\
	(n) & (1ULL << 13) ? 13 :												\
	(n) & (1ULL << 12) ? 12 :												\
	(n) & (1ULL << 11) ? 11 :												\
	(n) & (1ULL << 10) ? 10 :												\
	(n) & (1ULL <<  9) ?  9 :												\
	(n) & (1ULL <<  8) ?  8 :												\
	(n) & (1ULL <<  7) ?  7 :												\
	(n) & (1ULL <<  6) ?  6 :												\
	(n) & (1ULL <<  5) ?  5 :												\
	(n) & (1ULL <<  4) ?  4 :												\
	(n) & (1ULL <<  3) ?  3 :												\
	(n) & (1ULL <<  2) ?  2 : 1 )

#define ilog2(n)															\
(																			\
	__builtin_constant_p(n) ? ilog2_const(n) : ((sizeof(n) <= 4) ?			\
		__ilog2_u32(n) : __ilog2_u64(n))									\
)

/**
 * roundup_pow_of_two - round the given value up to nearest power of two
 * @n: parameter
 *
 * round the given value up to the nearest power of two
 * - the result is undefined when n == 0
 * - this can be used to initialise global variables from constant data
 */
#define roundup_pow_of_two(n)												\
(																			\
	__builtin_constant_p(n) ? ((n == 1) ? 1 : 								\
		(1UL << (ilog2((n) - 1) + 1))) : __roundup_pow_of_two(n)			\
)

/**
 * rounddown_pow_of_two - round the given value down to nearest power of two
 * @n: parameter
 *
 * round the given value down to the nearest power of two
 * - the result is undefined when n == 0
 * - this can be used to initialise global variables from constant data
 */
#define rounddown_pow_of_two(n)												\
(																			\
	__builtin_constant_p(n) ? ((1UL << ilog2(n))) :							\
		__rounddown_pow_of_two(n)											\
)

static inline int __order_base_2(unsigned long n)
{
	return n > 1 ? ilog2(n - 1) + 1 : 0;
}

/**
 * order_base_2 - calculate the (rounded up) base 2 order of the argument
 * @n: parameter
 *
 * The first few values calculated by this routine:
 *  ob2(0) = 0
 *  ob2(1) = 0
 *  ob2(2) = 1
 *  ob2(3) = 2
 *  ob2(4) = 2
 *  ob2(5) = 3
 *  ... and so on.
 */
#define order_base_2_const(n) 												\
	(((n) == 0 || (n) == 1) ? 0 : ilog2_const((n) - 1) + 1)

#define order_base_2(n)														\
(																			\
	__builtin_constant_p(n) ? order_base_2_const(n) : __order_base_2(n)		\
)

__END_DECLS

#endif
