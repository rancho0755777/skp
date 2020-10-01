//
// futex.c
// windows : CreateEvent
//
//
#include <time.h>
#include <sys/time.h>
#include <skp/utils/utils.h>
#include <skp/utils/futex.h>
#include <skp/process/thread.h>

//#if 0
#ifdef __linux__

#include <linux/futex.h>
# ifndef __NR_futex
#  define __NR_futex 202
# endif

# ifndef SYS_futex
#  define SYS_futex __NR_futex
# endif

# define __futex_op(addr1, op, val, rel, addr2, val3) \
	syscall(SYS_futex, addr1, op, val, rel, addr2, val3)

# ifdef FUTEX_WAKE_PRIVATE
#  define FUTEX__WAKE FUTEX_WAKE_PRIVATE
# else
#  define FUTEX__WAKE FUTEX_WAKE
# endif

# ifdef FUTEX_WAIT_PRIVATE
#  define FUTEX__WAIT FUTEX_WAIT_PRIVATE
# else
#  define FUTEX__WAIT FUTEX_WAIT
# endif

int futex_wait(int *uaddr, int val, int timeout)
{
	int flag = 0;
	struct timespec tm;
	struct timespec *tmptr = NULL;

	if (skp_likely(READ_ONCE(*uaddr) != val))
		return 1;

try:
	if (timeout >= 0) {
		tm.tv_sec = timeout / 1000;
		tm.tv_nsec = (timeout % 1000) * 1000 * 1000;
		tmptr = &tm;
	}

	wq_worker_sleeping();
	flag = __futex_op(uaddr, FUTEX__WAIT, val, tmptr, NULL, 0);
	wq_worker_waking_up();
	if (skp_unlikely(flag < 0)) {
		int code = errno;
		if (skp_likely(code == EWOULDBLOCK)) {
			/*value of uaddr has change*/
			return 1;
		} else if (skp_likely(code == ETIMEDOUT)) {
			return 0;
		} else if (skp_likely(code == EINTR)) {
			goto try;
		} else {
			/*内核系统版本太低，可能没有实现 futex ...*/
			BUG();
		}
	}

	return 1;
}

int futex_wake(int *uaddr, int n)
{
	int flag = 0;

	flag = __futex_op(uaddr, FUTEX__WAKE, n < 1 ? NR_CPUS : n, NULL, NULL, 0);
	BUG_ON(flag < 0);

	return flag;
}

#else
  #include <pthread.h>

/*
 * 离散使用条件变量的冲突
 * 只能通知到进程内部
 * 比起 futex 从本质上性能差了太多
 * TODO : 使用 以 唤醒地址为Key 和
 * 栈中的条件变量（等待实体）的链表为 Value 的Hash 表进行优化，
 * 防止惊群效应
 */

#define FUTEX_TALBE_SHIFT 6
#define FUTEX_TALBE_SIZE (1U << FUTEX_TALBE_SHIFT)

struct futex_entry {
	int waiter;
	pthread_cond_t cond;
	pthread_mutex_t mutex;
};

static struct futex_entry futex_table[] __cacheline_aligned = {
	[0 ... (FUTEX_TALBE_SIZE - 1)] = {
		0, PTHREAD_COND_INITIALIZER, PTHREAD_MUTEX_INITIALIZER
	},
};

static __always_inline struct futex_entry *acquire_entry(void *uaddr)
{
	BUILD_BUG_ON_NOT_POWER_OF_2(FUTEX_TALBE_SIZE);
	return futex_table + hash_ptr(uaddr, FUTEX_TALBE_SHIFT);
}

int futex_wait(int *uaddr, int val, int timeout)
{
	int rc = 1;
	struct futex_entry *entry = acquire_entry(uaddr);

	if (READ_ONCE(*uaddr) != val)
		return 1;

	wq_worker_sleeping();
	pthread_mutex_lock(&entry->mutex);
	entry->waiter++;
	while (READ_ONCE(*uaddr) == val) {
		int ret;
		struct timespec ts;
		similar_calendartime(&ts, timeout>=0?timeout:U32_MAX);
		ret = pthread_cond_timedwait(&entry->cond, &entry->mutex, &ts);
		if (skp_unlikely(ret > 0)) {
			WARN_ON(ret != ETIMEDOUT);
			rc = 0;
			break;
		}
	}
	entry->waiter--;
	pthread_mutex_unlock(&entry->mutex);
	wq_worker_waking_up();

	return rc;
}

int futex_wake(int *uaddr, int n)
{
	int waiter;
	struct futex_entry *entry = acquire_entry(uaddr);

	pthread_mutex_lock(&entry->mutex);
	waiter = READ_ONCE(entry->waiter);
	if (waiter > 1) {
		pthread_cond_broadcast(&entry->cond);
	} else if (waiter == 1) {
		pthread_cond_signal(&entry->cond);
	}
	pthread_mutex_unlock(&entry->mutex);

	return waiter;
}
#endif	/* ifdef __LINUX__ */

bool futex_cond_wait(int *uaddr, int until, int trys)
{
	do {
		int _old = READ_ONCE(*uaddr);
		if (skp_likely(_old == until))
			return true;
		futex_wait(uaddr, _old, 50);
	} while (trys--);

	return false;
}

