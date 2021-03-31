/*
 * @Author: kai.zhou
 * @Date: 2018-09-11 11:01:33
 */
#ifndef __SU_UTILS_H__
#define __SU_UTILS_H__

#include <fcntl.h>
#include <stdarg.h>
#include <strings.h>
#include <time.h>
#include <sys/time.h>

#include "config.h"
#include "hash.h"
#include "bug.h"
#include "bitops.h"
#include "atomic.h"
#include "bitmap.h"
#include "cpumask.h"

#ifdef __cplusplus
#include <algorithm>
#endif

__BEGIN_DECLS

#define U8_MAX		((uint8_t)~0U)
#define S8_MAX		((int8_t)(U8_MAX>>1))
#define S8_MIN		((int8_t)(-S8_MAX - 1))
#define U16_MAX		((uint16_t)~0U)
#define S16_MAX		((int16_t)(U16_MAX>>1))
#define S16_MIN		((int16_t)(-S16_MAX - 1))
#define U32_MAX		((uint32_t)~0U)
#define S32_MAX		((int32_t)(U32_MAX>>1))
#define S32_MIN		((int32_t)(-S32_MAX - 1))
#define U64_MAX		((uint64_t)~0ULL)
#define S64_MAX		((int64_t)(U64_MAX>>1))
#define U64_MIN		((int64_t)(-S64_MAX - 1))
////////////////////////////////////////////////////////////////////////////////
/** ARRAY_SIZE - get the number of elements in array @arr */
#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]) + __must_be_array(arr))
////////////////////////////////////////////////////////////////////////////////

/*
 * This looks more complex than it should be. But we need to
 * get the type for the ~ right in round_down (it needs to be
 * as wide as the result!), and we want to evaluate the macro
 * arguments just once each.
 */
#define __round_mask(x, y) ((__typeof__(x))((y)-1))
#define round_up(x, y) ((((x)-1) | __round_mask(x, y))+1)
#define round_down(x, y) ((x) & ~__round_mask(x, y))

/* The `const' in roundup() prevents gcc-3.3 from calling __divdi3 */
#define roundup(x, y)														\
	({ const typeof(y) __y = y;	 (((x) + (__y - 1)) / __y) * __y; })

#define rounddown(x, y)														\
	({ typeof(x) __x = (x);	 __x - (__x % (y)); })

////////////////////////////////////////////////////////////////////////////////
#define __typecheck(x, y) (!!(sizeof((typeof(x) *)1 == (typeof(y) *)1)))
/*
 * This returns a constant expression while determining if an argument is
 * a constant expression, most importantly without evaluating the argument.
 * Glory to Martin Uecker <Martin.Uecker@med.uni-goettingen.de>
 */
#define __cmp(x, y, op)	((x) op (y) ? (x) : (y))
#define __cmp_once(x, y, unique_x, unique_y, op)							\
({	typeof(x) unique_x = (x); typeof(y) unique_y = (y);						\
	__cmp(unique_x, unique_y, op);})

#ifndef __cplusplus
# define __is_constexpr(x)													\
	(sizeof(int) == sizeof(*(8 ? ((void *)((long)(x) * 0l)) : (int *)8)))
# define __no_side_effects(x, y) (__is_constexpr(x) && __is_constexpr(y))
# define __safe_cmp(x, y) (__typecheck(x, y) && __no_side_effects(x, y))
# define __careful_cmp(x, y, op)											\
	__builtin_choose_expr(__safe_cmp(x, y), __cmp(x, y, op),				\
	__cmp_once(x, y, __UNIQUE_ID(__x), __UNIQUE_ID(__y), op))
#else
# define __careful_cmp(x, y, op)											\
	__cmp_once(x, y, __UNIQUE_ID(__x), __UNIQUE_ID(__y), op)
#endif

#define swap_t(type, a, b) 													\
	do { type __tmp = (a); (a) = (b); (b) = __tmp; } while (0)

#ifndef __cplusplus
/** min - return minimum of two values of the same or compatible types */
#define min(x, y)	__careful_cmp(x, y, <)
/** max - return maximum of two values of the same or compatible types */
#define max(x, y)	__careful_cmp(x, y, >)
/** swap - swap values of @a and @b */
#define swap(a, b) swap_t(typeof(a), a, b)
#endif

/** min3 - return minimum of three values */
#define min3(x, y, z) min((typeof(x))min(x, y), z)

/** max3 - return maximum of three values */
#define max3(x, y, z) max((typeof(x))max(x, y), z)

/**
 * min_not_zero - return the minimum that is _not_ zero, unless both are zero
 */
#define min_not_zero(x, y)													\
({	typeof(x) __x = (x); typeof(y) __y = (y);								\
	__x == 0 ? __y : ((__y == 0) ? __x : min(__x, __y)); })

/**
 * clamp - return a value clamped to a given range with strict typechecking
 * This macro does strict typechecking of @lo/@hi to make sure they are of the
 * same type as @val.  See the unnecessary pointer comparisons.
 */
#define clamp(val, lo, hi) min((typeof(val))max(val, lo), hi)

/*
 * ..and if you can't take the strict
 * types, you can specify one yourself.
 *
 * Or not use min/max/clamp at all, of course.
 */

/** min_t - return minimum of two values, using the specified type */
#define min_t(type, x, y)	__careful_cmp((type)(x), (type)(y), <)

/** max_t - return maximum of two values, using the specified type */
#define max_t(type, x, y)	__careful_cmp((type)(x), (type)(y), >)

#define min3_t(type, x, y, z) min_t(type, min_t(type, x, y), z)

/** max3 - return maximum of three values */
#define max3_t(type, x, y, z) max_t(type, max_t(type, x, y), z)

/** clamp_t - return a value clamped to a given range using a given type */
#define clamp_t(type, val, lo, hi) min_t(type, max_t(type, val, lo), hi)

/** clamp_val - return a value clamped to a given range using val's type */
#define clamp_val(val, lo, hi) clamp_t(typeof(val), val, lo, hi)

////////////////////////////////////////////////////////////////////////////////
#ifndef PAGE_SIZE
/*必须为UL型*/
# define PAGE_SIZE (4096UL)
# define PAGE_MASK (~(4095UL))
#endif

#define BITS_PER_PAGE ((uint32_t)(PAGE_SIZE * 8))

#define PAGE_ALIGN(addr) ALIGN(addr, PAGE_SIZE)
#define CACHE_ALIGN(addr) ALIGN(addr, CACHELINE_BYTES)

#define PAGE_ALIGNED(addr) IS_ALIGNED(addr, PAGE_SIZE)
#define CACHE_ALIGNED(addr) IS_ALIGNED(addr, CACHELINE_BYTES)

////////////////////////////////////////////////////////////////////////////////
extern const char *__strerror_local(int code);
#define strerror_local() __strerror_local(errno)
#define syserrno_return(e) do { errno = e; return -1; } while (0)
////////////////////////////////////////////////////////////////////////////////
// 虚随机数
extern void prandom_seed(uint64_t seed);
extern uint32_t prandom(void);
/*闭区间整数随机值*/
static inline uint32_t prandom_int(unsigned long l, unsigned long h)
{
	BUG_ON(h < l);
	return ((uint32_t)((((float)prandom())/((float)U32_MAX))*(h-l+1))+l);
}
/*开区间浮点数随机值*/
static inline float prandom_real(uint32_t l, uint32_t h)
{
	BUG_ON(h < l);
	return ((float)((((float)prandom()) / ((float)U32_MAX)) * (h - l)) + l);
}

static inline bool prandom_chance(float p)
{
	return prandom_real(0, 1) < p ? true : false;
}

////////////////////////////////////////////////////////////////////////////////
#ifdef __x86_64__
/* Using 64-bit values saves one instruction clearing the high half of low */
# define DECLARE_ARGS(val, low, high)	unsigned long long low, high
# define EAX_EDX_VAL(val, low, high)	((low) | (high) << 32)
# define EAX_EDX_RET(val, low, high)	"=a" (low), "=d" (high)
#else
# define DECLARE_ARGS(val, low, high)	unsigned long long val
# define EAX_EDX_VAL(val, low, high)	(val)
# define EAX_EDX_RET(val, low, high)	"=A" (val)
#endif

/*获取指令周期 TSC*/
static inline unsigned long long rdtscll(void)
{
	DECLARE_ARGS(val, low, high);
	asm volatile("rdtsc" : EAX_EDX_RET(val, low, high));
	return EAX_EDX_VAL(val, low, high);
}

/*不可中断休眠*/
extern void usleep_unintr(uint32_t usecs);
#define msleep_unintr(msecs) usleep_unintr((msecs) * 1000)
#define sleep_unintr(secs) msleep_unintr((secs) * 1000)

/*获取时钟频率 : 每微秒的tick数量*/
extern uint32_t get_clockfreq(void);
/*获取时间戳*/
extern struct timespec *get_timestamp(struct timespec *ts);
/*获取近似时间戳*/
extern struct timespec *get_similar_timestamp(struct timespec *ts);
/*获取日历时间，可能被系统用户修改*/
extern struct timespec *get_calendar(struct timespec *ts);
/*获取近似日历时间，可能被系统用户修改*/
extern struct timespec *get_similar_calendar(struct timespec *ts);

static inline int64_t timestamp_diff(const struct timespec *op1,
	const struct timespec *op2, struct timespec *rs)
{
	struct timespec tmp;
	rs = rs?:&tmp;
	rs->tv_sec = op1->tv_sec - op2->tv_sec;
	rs->tv_nsec = op1->tv_nsec - op2->tv_nsec;
	if (rs->tv_nsec < 0) {
		rs->tv_sec -= 1;
		rs->tv_nsec += 1000000000;
	}
	return (int64_t)rs->tv_nsec + rs->tv_sec * 1000000000;
}

static inline uint64_t timestamp_offset(struct timespec *ts, long value)
{
	if (value) {
		ts->tv_sec += value / 1000;
		ts->tv_nsec += (value % 1000) * 1000000;
		if (ts->tv_nsec >= 1000000000) {
			ts->tv_sec += 1;
			ts->tv_nsec -= 1000000000;
		}
	}
	return (uint64_t)ts->tv_nsec + ts->tv_sec * 1000000000;
}

/*装载未来绝对时间*/
static inline uint64_t abstime(struct timespec *ts, long value)
{
	ts = get_timestamp(ts);
	return timestamp_offset(ts, value);
}

static inline uint64_t similar_abstime(struct timespec *ts, long value)
{
	ts = get_similar_timestamp(ts);
	return timestamp_offset(ts, value);
}

static inline uint64_t calendartime(struct timespec *ts, long value)
{
	ts = get_calendar(ts);
	return timestamp_offset(ts, value);
}

static inline uint64_t similar_calendartime(struct timespec *ts, long value)
{
	ts = get_similar_calendar(ts);
	return timestamp_offset(ts, value);
}

/*指令周期辅助函数*/
typedef unsigned long long cycles_t;
static inline cycles_t get_cycles(void) { return rdtscll(); }
/*流失的指令周期*/
static inline cycles_t escape_cycles(cycles_t s) { return rdtscll() - s; }
/*
 * CPU freq 2800 MHz
 * 2800 * 1M = 1s
 * 2.8 = 1ns
 * 1 = 1/2.8 ns
 */
static inline uint64_t cycles_to_ns(cycles_t v)
{
	return (uint64_t)v * 1000 / get_clockfreq();
}

#define cycles_to_us(v) (cycles_to_ns(v)/1000)
#define cycles_to_ms(v) (cycles_to_us(v)/1000)
#define cycles_to_sec(v) (cycles_to_ms(v)/1000)

/*时间字符串最大长度*/
#define TM_STRING_SIZE          (25)

/*时间格式 */
#define TM_FORMAT_DATE         "%Y-%m-%d"
#define TM_FORMAT_SHORT_DATE   "%y-%m-%d"
#define TM_FORMAT_TIME         "%H:%M:%S"
#define TM_FORMAT_DATETIME     "%Y-%m-%d %H:%M:%S"
/**
 * 格式时间
 */
extern char *format_time(char *tm_str, size_t size,
		time_t tm_int, const char *format);

static inline const char *format_current_time(const char *format)
{
	struct timespec *ts = get_similar_calendar(0);
	return format_time(0, 0, ts->tv_sec, format);
}
/**
 * 解析时间
 */
extern time_t parse_time(const char *tm_str, const char *format);
////////////////////////////////////////////////////////////////////////////////
extern int get_cpu_cores(void);
extern int thread_bind(int which);
extern int thread_cpu(void);
extern int get_thread_id(void);
extern int get_process_id(void);

////////////////////////////////////////////////////////////////////////////////
/*用于某些全局变量的非原子初始化函数，可休眠的全局递归锁*/
extern void big_lock(void);
extern void big_unlock(void);
////////////////////////////////////////////////////////////////////////////////
/*文件描述符标志*/
extern int enable_fd_flag(int fd, int flags);
extern int disable_fd_flag(int fd, int flags);

/*流文件阻塞标志操作*/
#define set_fd_nonblock(fd) enable_fd_flag((fd), O_NONBLOCK)
#define set_fd_block(fd) disable_fd_flag((fd), O_NONBLOCK)
#define set_fd_cloexec(fd) enable_fd_flag((fd), O_CLOEXEC)

#ifdef __apple__
extern int pipe2(int [2], int );
#endif

////////////////////////////////////////////////////////////////////////////////
// notifier function
////////////////////////////////////////////////////////////////////////////////

struct notifier {
	struct notifier *next;
	/*返回非0值将停止调用后续*/
	int (*action)(struct notifier *, void *);
};

#define NOTIFY_MAGIC	((struct notifier *)0x0000deadUL)
#define notifier_unlinked(b) skp_unlikely((b)->next == NOTIFY_MAGIC)

#define DEFINE_NOTIFIER(name, func)											\
	struct notifier name = { .action = func, .next = NOTIFY_MAGIC, }

#define DEFINE_NOTIFIER_HEADER(name) struct notifier *name = NULL

static inline void notifier_init(struct notifier *notifier,
		int (*action)(struct notifier *, void *))
{
	notifier->action = action;
	notifier->next = NOTIFY_MAGIC;
}
/**
 * @param stack 是否以栈的方式注册，先注册的后回调
 */
extern int notifier_register(struct notifier **, struct notifier *, bool stack);
extern int notifier_unregister(struct notifier **, struct notifier *);
/*触发*/
extern int notifier_invoke(struct notifier **, void *);

////////////////////////////////////////////////////////////////////////////////
/*同步监控文件变化*/
enum fchg {
	fchg_failed = -1,
	fchg_write = 1,
	fchg_attrib = 2,
	fchg_rename = 4,
	fchg_delete = 8,
	fchg_extend = 16, /*对于linux仅表示目标新建了文件，对于macos还表示文件被扩展*/
};

/*成功则返回 fchg_XXX 的集合*/
extern int file_changed(int fd, int timeout);

/*创建文件夹*/
extern int make_dir(const char *path, int mode);

__END_DECLS

#endif
