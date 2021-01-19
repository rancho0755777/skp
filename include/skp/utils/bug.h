/*
 * @Author: kai.zhou
 * @Date: 2018-09-11 11:01:33
 */
#ifndef __SU_BUG_H__
#define __SU_BUG_H__

#include "compiler.h"

__BEGIN_DECLS

extern int get_thread_id(void);
extern const char *get_timestamp_string(void);

#ifndef XX_MODULE_NAME
# define XX_MODULE_NAME "SKP"
#endif

#define LEVEL_ERR		"<1>"/* error conditions */
#define LEVEL_WARNING	"<2>"/* warning conditions */
#define LEVEL_INFO		"<3>"/* informational */
#define LEVEL_DEBUG		"<4>"/* debug-level messages */

#define __FILENAME__ ({                                     	\
	const char *__ptr, *__fname = __FILE__;                 	\
	(__ptr = strrchr(__fname, '/')) ? (__ptr + 1) : __fname;	\
})

#define ___x_log(LEVEL, log_type, fmt, file, line, func, ...)	\
	fprintf(stderr, LEVEL "%s, %5d, [" log_type "] "			\
		"[%10s] %15s:%4d[%-25s] - " fmt "\n",					\
		get_timestamp_string(), get_thread_id(), XX_MODULE_NAME,\
		file, line, func, ##__VA_ARGS__)

#define __x_log(LEVEL, log_type, fmt, ...)						\
	___x_log(LEVEL, log_type, fmt, __FILENAME__, __LINE__, 		\
		__FUNCTION__, ##__VA_ARGS__)

#define log_on(log_fn, cond, fmt, ...)							\
	do {if (cond) log_fn(fmt, ##__VA_ARGS__);} while (0)

#ifdef CONFIG_LOG_COLOR
# define info_color		"\x1B[34mInfo \x1B[0m"
# define warn_color		"\x1B[33mWarn \x1B[0m"
# define error_color	"\x1B[31mError\x1B[0m"
# define debug_color	"\x1B[32mDebug\x1B[0m"
#else
# define info_color		"Info "
# define warn_color		"Warn "
# define error_color	"Error"
# define debug_color	"Debug"
#endif

#define log_info(fmt, ...) __x_log(LEVEL_INFO, info_color, fmt, ##__VA_ARGS__)
#define log_warn(fmt, ...) __x_log(LEVEL_WARNING, warn_color, fmt, ##__VA_ARGS__)
#define log_error(fmt, ...)	\
	do { __x_log(LEVEL_ERR, error_color, fmt, ##__VA_ARGS__); } while (0)

#define log_info_on(condition, fmt, ...) log_on(log_info, condition, fmt, ##__VA_ARGS__)
#define log_warn_on(condition, fmt, ...) log_on(log_warn, condition, fmt, ##__VA_ARGS__)
#define log_error_on(condition, fmt, ...) log_on(log_error, condition, fmt, ##__VA_ARGS__)

#ifdef DEBUG
# define log_debug(fmt, ...) __x_log(LEVEL_DEBUG, debug_color, fmt, ##__VA_ARGS__)
# define log_debug_on(condition, fmt, ...) log_on(log_debug, condition, fmt, ##__VA_ARGS__)
#else
# define log_debug(fmt, ...) ((void)0)
# define log_debug_on(condition, fmt, ...) ((void)0)
#endif

#define painc(fmt, ...)	\
	do { log_error("PAINC " fmt, ##__VA_ARGS__); exit(1); } while (0)

#define painc_on(condition, fmt, ...) log_on(painc, condition, fmt, ##__VA_ARGS__)

//////////////////////////////////////////////////////////////////////////////////////////

/* Force a compilation error if a constant expression is not a power of 2 */
#define __BUILD_BUG_ON_NOT_POWER_OF_2(n)	\
	BUILD_BUG_ON(((n) & ((n) - 1)) != 0)
#define BUILD_BUG_ON_NOT_POWER_OF_2(n)			\
	BUILD_BUG_ON((n) == 0 || (((n) & ((n) - 1)) != 0))

/*为真则编译错误，整个表达式的值为 0*/
#define BUILD_BUG_ON_ZERO(e) (sizeof(struct { int:(-!!(e)); }))
#define BUILD_BUG_ON_MSG(cond, msg) compiletime_assert(!(cond), msg)

/*为真则编译错误*/
#ifndef __OPTIMIZE__
#define BUILD_BUG_ON(condition) ((void)sizeof(char[1 - 2*!!(condition)]))
#else
#define BUILD_BUG_ON(condition) \
	BUILD_BUG_ON_MSG(condition, "BUILD_BUG_ON failed: " #condition)
#endif

/**
 * BUILD_BUG - break compile if used.
 *
 * If you have some code that you expect the compiler to eliminate at
 * build time, you should use BUILD_BUG to detect if it is
 * unexpectedly used.
 */
#define BUILD_BUG() BUILD_BUG_ON_MSG(1, "BUILD_BUG failed")
/*TODO: c++类型静态比较
 *&a[0] degrades to a pointer: a different type from an array
 */
#ifdef __cpluscplus
# define __must_be_array(a)	0
#else
# define __must_be_array(a)	BUILD_BUG_ON_ZERO(__same_type((a), &(a)[0]))
#endif

#ifdef __unix__
# include <string.h>
# include <execinfo.h>
# define DUMP_STACK_BUFF_SIZE 1024
static inline void dump_stack(void)
{
	int    i, nptrs;
	void * buffer[DUMP_STACK_BUFF_SIZE];
	char **strs;

	nptrs = backtrace(buffer, DUMP_STACK_BUFF_SIZE);
	strs = backtrace_symbols(buffer, nptrs);

	for (i = 0; i < nptrs; i++)
		fprintf(stderr, "%s\n", strs[i]);
	/*防止 free 为一个宏*/
	(free)(strs);
}
#else
static inline void dump_stack(void) {}
#endif

/*todo : print info of stack*/

#define BUG()                         		\
	do {                              		\
		log_error("BUG and abort()"); 		\
		dump_stack();                 		\
		abort();                      		\
	} while (0)

#define WARN(cond, fmt, ...)          		\
	do {                              		\
		bool _c = !!(cond);           		\
		if (skp_unlikely(_c)) {           	\
			log_warn(fmt, ##__VA_ARGS__); 	\
		}                             		\
	} while (0)

#define BUG_ON(cond)                  		\
	do {                              		\
		bool _c = !!(cond);           		\
		if (skp_unlikely(_c)) {           	\
			log_error("BUG: " #cond); 		\
			dump_stack();             		\
			abort();                  		\
		}                             		\
	} while (0)

#define WARN_ON(cond)                 		\
	({                                		\
		bool _c = !!(cond);           		\
		if (skp_unlikely(_c)) {          	\
			log_warn("WARNING : %s", #cond);\
		}                             		\
		skp_unlikely(_c);                 	\
	})

//////////////////////////////////////////////////////////////////////////////////////////
/*
 * Kernel pointers have redundant information, so we can use a
 * scheme where we can return either an error code or a normal
 * pointer with the same return value.
 *
 * This should be a per-architecture thing, to allow different
 * error and pointer decisions.
 */
#define MAX_ERRNO 4095

#define IS_ERR_VALUE(x) skp_unlikely((unsigned long)(void *)(x) >= (unsigned long)-MAX_ERRNO)

static inline void *ERR_PTR(long error)
{
	return (void *)error;
}

static inline long PTR_ERR(const void *ptr)
{
	return (long)ptr;
}

static inline bool IS_ERR(const void *ptr)
{
	return IS_ERR_VALUE((unsigned long)ptr);
}

static inline bool IS_ERR_OR_NULL(const void *ptr)
{
	return skp_unlikely(!ptr) || IS_ERR_VALUE((unsigned long)ptr);
}

/**
 * ERR_CAST - Explicitly cast an error-valued pointer to another pointer type
 * @ptr: The pointer to cast.
 *
 * Explicitly cast an error-valued pointer to another pointer type in such a
 * way as to make it clear that's what's going on.
 */
static inline void *ERR_CAST(const void *ptr)
{
	/* cast away the const */
	return (void *)ptr;
}

static inline int PTR_ERR_OR_ZERO(const void *ptr)
{
	if (IS_ERR(ptr))
		return (int)PTR_ERR(ptr);
	else
		return 0;
}

__END_DECLS

#endif
