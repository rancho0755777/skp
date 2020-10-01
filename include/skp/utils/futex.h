//
//  futex.h
//  在非linux平台上只能用于线程间的同步，不能用于进程间的同步。
//
//  Created by kai.zhou on 18/9/18.
//
//

#ifndef __US_FUTEX_H__
#define __US_FUTEX_H__

#include "atomic.h"

__BEGIN_DECLS

/**
 * 在uaddr指向的这个地址上挂起等待（仅当*uaddr == cond 时，挂起）
 * @timeout 超时毫秒数，timeout < 0则挂起进程直到测试地址的值变化
 * @return 0 超时；1 被唤醒或挂起前*uaddr的值在挂起前已不等于val
 */
extern int futex_wait(int *uaddr, int cond, int timeout);

/**
 * 唤醒n个在uaddr指向的地址上挂起等待的进程
 * @return 被唤醒的进程数
 */
extern int futex_wake(int *uaddr, int n);
/**
 * fast user - condition, wait until [*uaddr] = [until]
 * the interval time is 1s for each test
 * @return false 失败
 */
extern bool futex_cond_wait(int *uaddr, int until, int trys);

/*
 * fast user - set signal, set [*uptr] = cond and wake up a waiter on [uptr]
 */
static inline bool futex_set_signal(int *uptr, int cond, int waiters)
{
	int old = -1;
	bool rc = false;
	old = xchg((uptr), (cond));
	if (skp_likely(old != (cond))) {
		futex_wake((uptr), (waiters));
		rc = true;
	}
	return rc;
}

/*
 * fast user - add signal, increment [*uptr]++  and wake up a waiter on [uptr]
 */
static inline void futex_add_signal(int *uptr, int inc, int waiters)
{
	xadd((uptr), (inc));
	futex_wake((uptr), (waiters));
}

/*
 * fast user - sub signal, decrease [*uptr]--  and wake up a waiter on [uptr]
 */
static inline void futex_sub_signal(int *uptr, int dec, int waiters)
{
	xadd((uptr), -(dec));
	futex_wake((uptr), (waiters));
}

__END_DECLS

#endif

