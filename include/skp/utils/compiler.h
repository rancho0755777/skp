/*
 * log tool
 * @Author: kai.zhou
 * @Date: 2018-09-11 11:01:33
 */
#ifndef __SU_COMPILER_H__
#define __SU_COMPILER_H__

/*
 * 平台检测
 */
#if !defined(__linux__) && (defined(__LINUX__) || defined(__KERNEL__) \
	|| defined(_LINUX) || defined(LINUX) || defined(__linux))
# define  __linux__    (1)
#elif !defined(__apple__) && (defined(__MacOS__) || defined(__APPLE__))
# define  __apple__    (1)
#elif !defined(__cygwin__) && (defined(__CYGWIN32__) || defined(CYGWIN))
# define  __cygwin__   (1)
#elif !defined(__mingw__) && (defined(__MINGW32__) || defined(MINGW))
# define  __mingw__   (1)
#elif !defined(__windows__) && (defined(_WIN32) || defined(WIN32) \
	|| defined(_window_) || defined(_WIN64) || defined(WIN64))
# define __windows__   (1)
#endif

#if !defined(__linux__) && !defined(__apple__)
# error "`not support this platform`"
#else
# define __unix__ 1
#endif

/*
 * Common definitions for all gcc versions go here.
 */
#define GCC_VERSION															\
	(__GNUC__ * 10000 + __GNUC_MINOR__ * 100 + __GNUC_PATCHLEVEL__)

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stddef.h>
#include <limits.h>
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/types.h>

#ifndef __BEGIN_DECLS
# if defined(__cplusplus)
#  define __BEGIN_DECLS	extern "C" {
#  define __END_DECLS	}
# else
#  define __BEGIN_DECLS
#  define __END_DECLS
# endif
#endif

__BEGIN_DECLS

#ifndef HZ
# define HZ (1000)
#endif

/*
 * intel x86 平台
 */
#if (__i386__ || __i386 || __amd64__ || __amd64)
# ifndef __x86__
#  define __x86__
# endif
#endif

#if (__amd64__ || __amd64) && !defined(__x86_64__)
# ifndef __x86_64__
#  define __x86_64__
# endif
#endif

#if __LP64__ 
# define BITS_PER_LONG 64
# define BITS_PER_LONG_SHIFT 6
#else
# define BITS_PER_LONG 32
# define BITS_PER_LONG_SHIFT 5
#endif

#ifndef BITS_PER_LONG_LONG
# define BITS_PER_LONG_LONG 64
# define BITS_PER_LONG_LONG_SHIFT 6
#endif

/* Optimization barrier */
/*
 * sfence ，实现Store Barrior 会将store buffer中缓存的修改刷入L1
 * 	cache中，使得其他cpu核可以观察到这些修改，而且之后的写操作不会被调度到之前，
 * 	即sfence之前的写操作一定在sfence完成且全局可见；
 * lfence ，实现Load Barrior 会将invalidate queue失效，强制读取入L1
 * 	cache中，而且lfence之后的读操作不会被调度到之前，
 *  即lfence之前的读操作一定在lfence完成（并未规定全局可见性）
 * mfence ，实现Full Barrior 同时刷新store buffer和invalidate
 *  queue，保证了mfence前后的读写操作的顺序，同时要求mfence之后写操作结果全局可见之前，
 * 	即mfence之前写操作结果全局可见；
 * lock 用来修饰当前指令操作的内存只能由当前CPU使用，若指令不操作内存仍然由用，
 *  因为这个修饰会让指令操作本身原子化，而且自带Full Barrior效果；
 *  还有指令比如IO操作的指令、exch等原子交换的指令，
 *  任何带有lock前缀的指令以及CPUID等指令都有内存屏障的作用。
 */
/* The "volatile" is due to gcc bugs */
#define static_mb() asm volatile("": : :"memory")
#define smp_mb() 	asm volatile("mfence":::"memory")
#define smp_rmb()	asm volatile("lfence":::"memory")
#define smp_wmb()	asm volatile("sfence" ::: "memory")

#ifndef __always_inline
# define __always_inline	inline __attribute__((always_inline))
#endif

#define __aligned(x)		__attribute__((aligned(x)))
#define __aligned_largest	__attribute__((aligned))
#define __aligned_packed	__attribute__((packed))
#define __printf(a, b)		__attribute__((format(printf, a, b)))
#define __scanf(a, b)		__attribute__((format(scanf, a, b)))
#define __maybe_unused		__attribute__((unused))
#define __always_unused		__attribute__((unused))

#define prefetch(x)		__builtin_prefetch(x)
#define prefetchw(x)	__builtin_prefetch(x, 1)

#ifndef skp_likely
# define skp_likely(x)		__builtin_expect(!!(x), 1)
# define skp_unlikely(x)	__builtin_expect(!!(x), 0)
#endif

////////////////////////////////////////////////////////////////////////////////
#define cpu_relax() __asm__("pause")
#define cpu_nop() __asm__("rep; nop")

#if GCC_VERSION >= 40000

#define __compiler_offsetof(a, b)	__builtin_offsetof(a, b)

#if GCC_VERSION >= 40100
# define __compiletime_object_size(obj) __builtin_object_size(obj, 0)
#endif

#if GCC_VERSION >= 40200
/* Mark functions as cold. gcc will assume any path leading to a call
 * to them will be skp_unlikely.  This means a lot of manual skp_unlikely()s
 * are unnecessary now for any paths leading to the usual suspects
 * like BUG(), printk(), panic() etc. [but let's keep them for now for
 * older compilers]
 */
#define __cold			__attribute__((__cold__))

#define ___PASTE(a, b) a ## b
#define __PASTE(a, b) ___PASTE(a, b)

#define __UNIQUE_ID(prefix) __PASTE(__PASTE(__UNIQUE_ID_, prefix), __COUNTER__)

#ifndef __same_type
# define __same_type(a, b) __builtin_types_compatible_p(typeof(a), typeof(b))
#endif

#ifndef __native_word
# define __native_word(t) (sizeof(t) == sizeof(char) ||			\
	sizeof(t) == sizeof(short) || sizeof(t) == sizeof(int) ||	\
	sizeof(t) == sizeof(long))
#endif

#ifndef L1_CACHE_SHIFT
# define L1_CACHE_SHIFT 7
#endif

#ifndef CACHELINE_BYTES
# define CACHELINE_BYTES (1 << L1_CACHE_SHIFT)
#endif

#define __cacheline_aligned __aligned(CACHELINE_BYTES)

#define CONFIG_LARGE_STRUCT

#ifdef CONFIG_LARGE_STRUCT
struct __padding {
	char _[1];
} __cacheline_aligned;
# define PADDING(name) struct __padding name;
#else
# define PADDING(name)
#endif

////////////////////////////////////////////////////////////////////////////////
#if GCC_VERSION >= 70000
# ifndef __CHECKER__
#  define __compiletime_warning(message) __attribute__((warning(message)))
#  define __compiletime_error(message) __attribute__((error(message)))
# endif /* __CHECKER__ */
#endif /* GCC_VERSION >= 70000 */

////////////////////////////////////////////////////////////////////////////////

#endif /* GCC_VERSION >= 40300 */
#endif /* GCC_VERSION >= 40000 */

/*marco __OPTIMIZE__ will be auto defined when use -O1+ option to compile file*/

#ifndef __compiletime_error
# define __compiletime_error(message)
#endif

#ifndef __compiletime_warning
# define __compiletime_warning(message)
#endif

#define __compiletime_error_fallback(condition) 						\
	do { ((void)sizeof(char[1 - 2 * condition])); } while (0)

#if GCC_VERSION >=70000 && defined(__OPTIMIZE__)
# define __compiletime_assert(condition, msg, prefix, suffix)			\
	do {																\
		bool __cond = !(condition);										\
		extern void prefix ## suffix(void) __compiletime_error(msg);	\
		if (__cond)														\
			prefix ## suffix();											\
		__compiletime_error_fallback(__cond);							\
	} while (0)
#else
# define __compiletime_assert(condition, msg, prefix, suffix)			\
	__compiletime_error_fallback(!(condition))
#endif

#define _compiletime_assert(condition, msg, prefix, suffix)				\
	__compiletime_assert(condition, msg, prefix, suffix)

/**
 * compiletime_assert - break build and emit msg if condition is false
 * @condition: a compile-time constant condition to check
 * @msg:       a message to emit if condition is false
 *
 * In tradition of POSIX assert, this macro will break the build if the
 * supplied condition is *false*, emitting the supplied error message if the
 * compiler has support to do so.
 */
#define compiletime_assert(condition, msg) 								\
	_compiletime_assert(condition, msg, __compiletime_assert_, __LINE__)

#define compiletime_assert_atomic_type(t)								\
	compiletime_assert(__native_word(t),								\
		"Need native word sized stores/loads for atomicity.")

////////////////////////////////////////////////////////////////////////////////
#ifdef CONFIG_CPU_BIG_ENDIAN
# define __BIG_ENDIAN 4321
#else
# define __LITTLE_ENDIAN 1234
#endif
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
#define MAGIC_U8  0xddU
#define MAGIC_U16 0xdeadU
#define MAGIC_U32 0xdeadbeefU
#define MAGIC_U64 0xdeadbeefdeadbeefULL

__END_DECLS
////////////////////////////////////////////////////////////////////////////////
#endif
