/*
 * @Author: kai.zhou
 * @Date: 2019-04-20 11:01:33
 */
#ifndef __SU_SEQLOCK_H__
#define __SU_SEQLOCK_H__

#include "spinlock.h"

/*
 * Reader/writer consistent mechanism without starving writers. This type of
 * lock for data where the reader wants a consitent set of information
 * and is willing to retry if the information changes.  Readers never
 * block but they may have to retry if a writer is in
 * progress. Writers do not wait for readers. 
 *
 * This is not as cache friendly as brlock. Also, this will not work
 * for data that contains pointers, because any writer could
 * invalidate a pointer that a reader was following.
 *
 * Expected reader usage:
 * 	do {
 *	    seq = read_seqbegin(&foo);
 * 		...
 *  } while (read_seqretry(&foo, seq));
 *
 *
 * On non-SMP the spin locks disappear but the writer still needs
 * to increment the sequence variables because an interrupt routine could
 * change the state of the data.
 *
 * Based on x86_64 vsyscall gettimeofday 
 * by Keith Owens and Andrea Arcangeli
 */

__BEGIN_DECLS

/*
 * 顺序锁，写操作非常的频率非常低才适用
 * 写锁的优先级大于读锁，读者需要反复的测试顺序锁是否发生了变化
 * 来判断是否发生 过 写操作
 */
typedef struct {
	unsigned sequence;/**<为了提醒读者 数据是否变化*/
	spinlock_t lock;/**<为了让写者互斥*/
} seqlock_t;

/*
 * These macros triggered gcc-3.x compile-time problems.  We think these are
 * OK now.  Be cautious.
 */
#define SEQLOCK_UNLOCKED(n) { 0, __SPIN_LOCK_INITIALIZER(n), }
#define seqlock_init(x)	do { *(x) = (seqlock_t)SEQLOCK_UNLOCKED(x); } while (0)
#define DECLARE_SEQLOCK(name) seqlock_t name
#define DEFINE_SEQLOCK(name) seqlock_t name = SEQLOCK_UNLOCKED(name)

/* Lock out other writers and update the count.
 * Acts like a normal spin_lock/unlock.
 * Don't need preempt_disable() because that is in the spin_lock already.
 * 进行一次写后，sequence字段必然是偶数
 */
static inline void write_seqlock(seqlock_t *sl)
{
	spin_lock(&sl->lock);
	++sl->sequence;
	smp_mb();
}	

static inline void write_sequnlock(seqlock_t *sl) 
{
	smp_mb();
	sl->sequence++;
	spin_unlock(&sl->lock);
}

static inline int write_tryseqlock(seqlock_t *sl)
{
	int ret = spin_trylock(&sl->lock);

	if (ret) {
		++sl->sequence;
		smp_mb();
	}
	return ret;
}

/* Start of read calculation -- fetch last complete writer token */
static inline unsigned read_seqbegin(const seqlock_t *sl)
{
	unsigned ret = READ_ONCE(sl->sequence);
	smp_mb();
	return ret;
}

/** @return true  需要重试*/
static inline bool read_seqretry(const seqlock_t *sl, unsigned iv)
{
	smp_mb();
	return !!((iv & 1) | (READ_ONCE(sl->sequence) ^ iv));
}

/*
 * Version using sequence counter only.
 * This can be used when code has its own mutex protecting the
 * updating starting before the write_seqcountbeqin() and ending
 * after the write_seqcount_end().
 */
typedef struct seqcount {
	unsigned sequence;
} seqcount_t;

#define SEQCNT_ZERO { 0 }
#define seqcount_init(x)	do { *(x) = (seqcount_t) SEQCNT_ZERO; } while (0)
#define DECLARE_SEQCOUNT(name) seqcount_t name
#define DEFINE_SEQCOUNT(name) seqcount_t name = SEQCNT_ZERO

/* Start of read using pointer to a sequence counter only.  */
static inline unsigned read_seqcount_begin(const seqcount_t *s)
{
	unsigned ret = s->sequence;
	smp_mb();
	return ret;
}

/* Test if reader processed invalid data.
 * Equivalent to: iv is odd or sequence number has changed.
 *                (iv & 1) || (*s != iv)
 * Using xor saves one conditional branch.
 * @return true  需要重试
 */
static inline bool read_seqcount_retry(const seqcount_t *s, unsigned iv)
{
	smp_mb();
	return !!((iv & 1) | (s->sequence ^ iv));
}

/*
 * Sequence counter only version assumes that callers are using their
 * own mutexing.
 */
static inline void write_seqcount_begin(seqcount_t *s)
{
	s->sequence++;
	smp_mb();
}

static inline void write_seqcount_end(seqcount_t *s)
{
	smp_mb();
	s->sequence++;
}

__END_DECLS

#endif
