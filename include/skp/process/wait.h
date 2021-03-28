#ifndef __SU_WAIT_H__
#define __SU_WAIT_H__

#include "../utils/spinlock.h"
#include "../adt/list.h"

__BEGIN_DECLS

/* !!! wait 实体不能共享 !!! */
typedef struct __wait_queue wait_queue_t;
typedef struct __wait_queue_head wait_queue_head_t;
/**@return 返回 0，表示唤醒条件不成立，继续查看 queue 中其他 wait*/
typedef int (*wake_queue_fn)(wait_queue_t *wait, void *key);
/**@return 返回 非 0，表示等待条件不成立，终止等待*/
typedef int (*wait_queue_fn)(wait_queue_t *wait);

/**@return > 0 : success wake up process*/
extern int default_wake_function(wait_queue_t *wait, void *key);
extern int autoremove_wake_function(wait_queue_t *wait, void *key);

/*这个结构体大多数都在栈中，没有必要对空间进行优化*/
#define WQ_FLAG_EXCLUSIVE 0x01
#define WQ_COND_STRUCT						\
	union {									\
		int32_t value;						\
		struct {							\
			int16_t cond;					\
			int16_t last_cond;				\
		};									\
	}

struct __wait_queue {
	WQ_COND_STRUCT;
	uint32_t flags;
	wake_queue_fn func; /*用于唤醒*/
	struct list_head task_list;
};

struct __wait_queue_head {
	spinlock_t lock;
	struct list_head task_list;
};

#define __WAITQUEUE_INITIALIZER(name, condition, function)	\
{															\
	.flags = 0, .last_cond = (condition) - 1,				\
	.cond = (condition), .func = (function),				\
	.task_list = LIST_HEAD_INIT(name.task_list),			\
}

#define DEFINE_WAITQUEUE(name)								\
	wait_queue_t name = __WAITQUEUE_INITIALIZER(name, 0, default_wake_function)

#define DEFINE_AUTOREMOVE_WAITQUEUE(name)					\
	wait_queue_t name = __WAITQUEUE_INITIALIZER(name, 0, autoremove_wake_function)

#define __WAIT_QUEUE_HEAD_INITIALIZER(name)					\
{															\
	.lock = __SPIN_LOCK_INITIALIZER(name),					\
	.task_list = LIST_HEAD_INIT(name.task_list),			\
}

#define DEFINE_WAIT_QUEUE_HEAD(name)						\
	wait_queue_head_t name = __WAIT_QUEUE_HEAD_INITIALIZER(name)

static inline void init_waitqueue_head(wait_queue_head_t *q)
{
	spin_lock_init(&q->lock);
	INIT_LIST_HEAD(&q->task_list);
}

static inline void init_waitqueue_entry(wait_queue_t *q)
{
	q->flags = 0;
	q->last_cond = -1;
	q->cond = 0;
	q->func = default_wake_function;
	INIT_LIST_HEAD(&q->task_list);
}

static inline
void __add_wait_queue(wait_queue_head_t *wq, wait_queue_t *wait)
{
	WRITE_ONCE(wait->last_cond, wait->cond);
	list_add(&wait->task_list, &wq->task_list);
}

static inline
void __add_wait_queue_tail(wait_queue_head_t *wq, wait_queue_t *wait)
{
	WRITE_ONCE(wait->last_cond, wait->cond);
	list_add_tail(&wait->task_list, &wq->task_list);
}

#define __remove_wait_queue(head, wait) list_del_init(&(wait)->task_list)
#define waitqueue_active(wq) ({smp_mb();!list_empty_careful(&(wq)->task_list);})

/*
 * usage like this:
 *     DEFINE_WAITQUEUE(wait);
 *     add_wait_queue(queue, &wait);
 *     while (!cond) {
 *         wait_on(&wait);
 *     }
 *     remove_wati_queue(queue, &wait);
 */
extern void add_wait_queue(wait_queue_head_t *q, wait_queue_t *wait);
extern void add_wait_queue_exclusive(wait_queue_head_t *q, wait_queue_t *wait);
extern void remove_wait_queue(wait_queue_head_t *q, wait_queue_t *wait);

extern void __wake_up(wait_queue_head_t *q, int nr, void *key);
#define __wake_up_one(q, key) __wake_up((q), 1, (key))
#define __wake_up_all(q, key) __wake_up((q), -1, (key))
#define wake_up_one(q) __wake_up_one((q), NULL)
#define wake_up_all(q) __wake_up((q), -1, NULL)

/**@return 0 timedout , > 0 success, < 0 error*/
extern int wait_on_timeout(wait_queue_t *wait, int timedout);
#define wait_on(wait) wait_on_timeout((wait), -1)

/*
 * usage like this:
 *     DEFINE_AUTOREMOVE_WAITQUEUE(wait)
 *     do {
 *         prepare_to_wait(queue, &wait);
 *         if (cond)
 *             break;
 *         wait_on(&wait);
 *     } while(!cond);
 *     finish_wait(queue, &wait);
 */
extern void prepare_to_wait(wait_queue_head_t *q, wait_queue_t *wait);
extern void prepare_to_wait_exclusive(wait_queue_head_t *q, wait_queue_t *wait);
extern void finish_wait(wait_queue_head_t *q, wait_queue_t *wait);

#define wait_queue_head_lock(h) (spin_lock(&(h)->lock))
#define wait_queue_head_unlock(h) (spin_unlock(&(h)->lock))

////////////////////////////////////////////////////////////////////////////////
// 使用外部锁时，使用下列函数
////////////////////////////////////////////////////////////////////////////////
static inline
void add_wait_queue_locked(wait_queue_head_t *q, wait_queue_t *wait)
{
	wait->flags &= ~ WQ_FLAG_EXCLUSIVE;
	__add_wait_queue(q, wait);
}

static inline
void add_wait_queue_exclusive_locked(wait_queue_head_t *q, wait_queue_t *wait)
{
	wait->flags |= WQ_FLAG_EXCLUSIVE;
	__add_wait_queue_tail(q, wait);
}

#define remove_wait_queue_locked(q, wait) __remove_wait_queue((q), (wait))

extern void __wake_up_locked(wait_queue_head_t *q, int nr, void *key);
#define __wake_up_one_locked(q, key) __wake_up_locked((q), 1, (key))
#define wake_up_one_locked(q) __wake_up_locked((q), 1, NULL)
#define wake_up_all_locked(q) __wake_up_locked((q), -1, NULL)

////////////////////////////////////////////////////////////////////////////////
// 快捷 等待
////////////////////////////////////////////////////////////////////////////////
/*等待条件为真*/
#define __wait_event(wq_head, cond, excl, cmd)				\
({															\
	int __rc = 1;											\
	DEFINE_AUTOREMOVE_WAITQUEUE(WAIT);						\
	do {													\
		if (excl)											\
			prepare_to_wait_exclusive((wq_head), &WAIT);	\
		else												\
			prepare_to_wait((wq_head), &WAIT);				\
		if (!!(cond))										\
			break;											\
		__rc = cmd;											\
	} while (__rc > 0 && !(cond));							\
	finish_wait((wq_head), &WAIT);							\
	__rc;													\
})


#define __wait_event_lock(wq_head, cond, lock, cmd) 		\
	__wait_event((wq_head), (cond), false, ({				\
		spin_unlock((lock));								\
		cmd;												\
		wait_on(&WAIT);										\
		spin_lock((lock));									\
		1;													\
	}))

#define wait_event_lock(wq_head, cond, lock)				\
	do {													\
		if ((cond))											\
			break;											\
		__wait_event_lock((wq_head), (cond), (lock), );		\
	} while(0)

#define wait_event(wq_head, cond)							\
	do {													\
		if ((cond))											\
			break;											\
		__wait_event((wq_head), (cond), false,				\
			({wait_on(&WAIT); 1;}));						\
	} while(0)

////////////////////////////////////////////////////////////////////////////////
// 位等待队列
////////////////////////////////////////////////////////////////////////////////
#define wait_bit_queue_from_wait(w) \
	container_of((w), wait_bit_queue_t, wait)
typedef struct __wait_bit_queue wait_bit_queue_t;

struct __wait_bit_key {
	void *flags; /*bitmap*/
	int bit_nr; /*bit*/
};

struct __wait_bit_queue {
	struct __wait_bit_key key;
	wait_queue_t wait;
};

#define __WAIT_BIT_KEY_INITIALIZER(word, bit)				\
	{ .flags = word, .bit_nr = bit, }

#define DEFINE_WAITBIT(name, word, bit) \
	wait_bit_queue_t name = { \
		.key = __WAIT_BIT_KEY_INITIALIZER(word, bit), \
		.wait = __WAITQUEUE_INITIALIZER(name.wait, 0, wake_bit_function), \
	}

extern wait_queue_head_t *bit_waitqueue(void *, int);
extern int wake_bit_function(wait_queue_t *wait, void *key);

/**
 * @param int(*action)(wait_queue_t *) return > 0 then break waiting
 */
extern int __wait_on_bit_common(wait_queue_head_t *wq, wait_bit_queue_t *q,
		wait_queue_fn action, bool lock);
#define __wait_on_bit(wq, q, fn) __wait_on_bit_common((wq),(q),(fn),false)
#define __wait_on_bit_lock(wq, q, fn) __wait_on_bit_common((wq),(q),(fn),true)

extern void __wake_up_bit(wait_queue_head_t *, void *, int);
extern void wake_up_bit(void *, int);
extern int out_of_line_wait_on_bit(void *, int, wait_queue_fn);
extern int out_of_line_wait_on_bit_lock(void *, int, wait_queue_fn);

/**
 * 等待位清除
 * @return 0 success, else error
 * @wait_queue_fn use to waitting on queue
 */
static inline int wait_on_bit(void *word, int bit, wait_queue_fn action)
{
	if (!test_bit(bit, word))
		return 0;
	return out_of_line_wait_on_bit(word, bit, action);
}

/**
 * 抢占位，如果需要则等待
 * @return 0 success, else error
 * otherwise wait and try again until success or interrupte
 */
static inline int wait_on_bit_lock(void *word, int bit, wait_queue_fn action)
{
	if (!test_and_set_bit(bit, (volatile unsigned long *)word))
		return 0;
	return out_of_line_wait_on_bit_lock(word, bit, action);
}

__END_DECLS

#endif
