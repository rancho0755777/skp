#include <skp/process/wait.h>
#include <skp/utils/futex.h>

static inline int __default_wake_function(wait_queue_t *wait, void *key)
{
	xadd(&wait->cond, 1);
	return futex_wake(&wait->cond, 1) ? 1 : 0;
}

static inline int __autoremove_wake_function(wait_queue_t *wait, void *key)
{
	int rc  = __default_wake_function(wait, key);
	if (skp_likely(rc))
		__remove_wait_queue(NULL, wait);
	return rc;
}

int default_wake_function(wait_queue_t *wait, void *key)
{
	return __default_wake_function(wait, key);
}

int autoremove_wake_function(wait_queue_t *wait, void *key)
{
	return __autoremove_wake_function(wait, key);
}

int wake_bit_function(wait_queue_t *wait, void *arg)
{
	struct __wait_bit_key *key = arg;
	wait_bit_queue_t *wait_bit = container_of(wait, wait_bit_queue_t, wait);
	/*位不匹配或位仍未解锁，多个位图使用相同的队列，需要做关键字匹配检查*/
	if (wait_bit->key.flags != key->flags ||
			wait_bit->key.bit_nr != key->bit_nr ||
			test_bit(key->bit_nr, key->flags))
		return 0;
	return __autoremove_wake_function(wait, key);
}

static void ___wake_up_common(wait_queue_head_t *q, int nr_exclusive, void *key)
{
	uint32_t flags;
	wait_queue_t *curr, *next;
	list_for_each_entry_safe(curr, next, &q->task_list, task_list) {
		flags = curr->flags;
		/*func 返回 0 表示 条件不匹配，继续操作其他的 waiter */
		if (curr->func(curr, key) &&
				(flags & WQ_FLAG_EXCLUSIVE) && !--nr_exclusive)
			break;
	}
}

static inline void ___wake_up(wait_queue_head_t *q, int nr, void *key)
{
	spin_lock(&q->lock);
	___wake_up_common(q, nr, key);
	spin_unlock(&q->lock);
}

void add_wait_queue(wait_queue_head_t *q, wait_queue_t *wait)
{
	spin_lock(&q->lock);
	add_wait_queue_locked(q, wait);
	spin_unlock(&q->lock);
}

void add_wait_queue_exclusive(wait_queue_head_t *q, wait_queue_t *wait)
{
	spin_lock(&q->lock);
	add_wait_queue_exclusive_locked(q, wait);
	spin_unlock(&q->lock);
}

void remove_wait_queue(wait_queue_head_t *q, wait_queue_t *wait)
{
	spin_lock(&q->lock);
	remove_wait_queue_locked(q, wait);
	spin_unlock(&q->lock);
}

void prepare_to_wait(wait_queue_head_t *q, wait_queue_t *wait)
{
	wait->flags &= ~ WQ_FLAG_EXCLUSIVE;
	spin_lock(&q->lock);
	if (list_empty(&wait->task_list))
		__add_wait_queue(q, wait);
	spin_unlock(&q->lock);
}

void prepare_to_wait_exclusive(wait_queue_head_t *q, wait_queue_t *wait)
{
	wait->flags |= WQ_FLAG_EXCLUSIVE;
	spin_lock(&q->lock);
	if (list_empty(&wait->task_list))
		__add_wait_queue_tail(q, wait);
	spin_unlock(&q->lock);
}

void finish_wait(wait_queue_head_t *q, wait_queue_t *wait)
{
	if (!list_empty_careful(&wait->task_list)) {
		spin_lock(&q->lock);
		__remove_wait_queue(q, wait);
		spin_unlock(&q->lock);
	}
}

void __wake_up(wait_queue_head_t *q, int nr, void *key)
{
	___wake_up(q, nr, key);
}

void __wake_up_locked(wait_queue_head_t *q, int nr, void *key)
{
	___wake_up_common(q, nr, key);
}

int wait_on_timeout(wait_queue_t *wait, int timedout)
{
	int rc = 1, cond = READ_ONCE(wait->cond);
	if (cond == READ_ONCE(wait->last_cond))
		rc = futex_wait(&wait->cond, cond, timedout);
	wait->last_cond = READ_ONCE(wait->cond);
	return rc;
}

static inline wait_queue_head_t *__bit_waitqueue(void *word, int bit);

int __wait_on_bit_common(wait_queue_head_t *wq, wait_bit_queue_t *q,
		wait_queue_fn action, bool lock)
{
	int ret = 0;

	q->wait.flags = lock ? WQ_FLAG_EXCLUSIVE : 0;
	do {
		wait_queue_head_lock(wq);
		if (list_empty(&q->wait.task_list)) {
			if (lock) {
				__add_wait_queue_tail(wq, &q->wait);
			} else {
				__add_wait_queue(wq, &q->wait);
			}
		}
		wait_queue_head_unlock(wq);

		if (test_bit(q->key.bit_nr, q->key.flags)) {
			if ((ret = action(&q->wait)))
				break;
		}

		if (lock) {
			ret = test_and_set_bit(q->key.bit_nr, q->key.flags);
		} else {
			ret = test_bit(q->key.bit_nr, q->key.flags);
		}
	} while (ret);
	finish_wait(wq, &q->wait);
	return ret;
}

int out_of_line_wait_on_bit(void *word, int bit, wait_queue_fn action)
{
	wait_queue_head_t *wq = __bit_waitqueue(word, bit);
	DEFINE_WAITBIT(wait, word, bit);
	return __wait_on_bit(wq, &wait, action);
}

int out_of_line_wait_on_bit_lock(void *word, int bit, wait_queue_fn action)
{
	wait_queue_head_t *wq = __bit_waitqueue(word, bit);
	DEFINE_WAITBIT(wait, word, bit);
	return __wait_on_bit_lock(wq, &wait, action);
}

void __wake_up_bit(wait_queue_head_t *wq, void *word, int bit)
{
	struct __wait_bit_key key = __WAIT_BIT_KEY_INITIALIZER(word, bit);
	if (waitqueue_active(wq))
		___wake_up(wq, 1, &key);
}

void wake_up_bit(void *word, int bit)
{
	__wake_up_bit(__bit_waitqueue(word, bit), word, bit);
}

#define WAIT_TABLE_BITS 6
#define WAIT_TABLE_SIZE (1 << WAIT_TABLE_BITS)
static bool bit_wait_up = false;
static wait_queue_head_t bit_wait_table[WAIT_TABLE_SIZE] __cacheline_aligned;

static inline wait_queue_head_t *__bit_waitqueue(void *word, int bit)
{
	const int shift = BITS_PER_LONG == 32 ? 5 : 6;
	unsigned long val = (unsigned long)word << shift | bit;

	if (skp_unlikely(!READ_ONCE(bit_wait_up))) {
		BUILD_BUG_ON_NOT_POWER_OF_2(ARRAY_SIZE(bit_wait_table));
		/*optimistic locking*/
		big_lock();
		if (skp_likely(!bit_wait_up)) {
			for (int i = 0; i < WAIT_TABLE_SIZE; i++) {
				init_waitqueue_head(&bit_wait_table[i]);
			}
			WRITE_ONCE(bit_wait_up, true);
		}
		big_unlock();
	}
	return bit_wait_table + hash_long(val, WAIT_TABLE_BITS);
}

wait_queue_head_t *bit_waitqueue(void *word, int bit)
{
	return __bit_waitqueue(word, bit);
}
