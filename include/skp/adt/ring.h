#ifndef __US_RING_H__
#define __US_RING_H__

/*从DPDK移植的 无锁 ring buff */

#include "../utils/utils.h"
#include "../utils/atomic.h"

__BEGIN_DECLS

#define RINGB_F_SP_ENQ 0x0001
#define RINGB_F_SC_DEQ 0x0002
#define RINGB_F_EXACT_SZ 0x0004 /**< 参数给出容量即为可用容量*/

#define RINGB_M_SP 1
#define RINGB_M_MP 0
#define RINGB_M_SC 1
#define RINGB_M_MC 0

#define RINGB_Q_FIXED 0
#define RINGB_Q_VARIABLE 1

struct ringb_headtail {
	volatile uint32_t head;  /**< 生产者或消费者头指针. */
	volatile uint32_t tail;  /**< 生产者或消费者尾指针. */
	uint32_t single;         /**< 为真则默认出入队列为非线程安全 */
};

struct ringb {
	int flags;               /**< 标志位，控制默认行为，RINGB_F_XXX 组合. */
	uint32_t size;           /**< ring 实际容量. */
	uint32_t mask;           /**< ring 容量掩码. */
	uint32_t capacity;       /**< ring 可用容量*/
	void * user;			 /**< 用户 数据*/

	PADDING(_pad0);

	/** 生产者状态. */
	struct ringb_headtail prod __cacheline_aligned;
	/** 消费者状态. */
	struct ringb_headtail cons __cacheline_aligned;

	char ring[0];			/**< buff 起始地址*/
} __cacheline_aligned;

/**
 * 计算需要分配的空间大小
 * @see 参数见 ringb_init()
 */
extern ssize_t ringb_calc_memsize(uint32_t count, uint32_t flags);

/**
 * 初始化
 * @param count 容量，如果 flags 没有 RINGB_F_EXACT_SZ 则必须为 2^n 倍数，
 * 且可用空间为 count - 1，否则 如果 RINGB_F_EXACT_SZ 置位，则容量就是 count
 * 此时 count 最好不要是 2^n 倍数，否则浪费很大的空间
 * @param flags RINGB_F_XXX 的组合
 */
extern int ringb_init(struct ringb *r, uint32_t count, uint32_t flags);

/**
 * 内部分配空间并创建 ring buff
 */
extern struct ringb *ringb_create(uint32_t count, uint32_t flags);

/**
 * 释放 ringb_create() 的描述符
 */
extern void ringb_free(struct ringb *r);

/**
 */
static inline uint32_t ringb_count(const struct ringb *r)
{
	uint32_t prod_tail = r->prod.tail;
	uint32_t cons_tail = r->cons.tail;
	uint32_t count = (prod_tail - cons_tail) & r->mask;
	return (count > r->capacity) ? r->capacity : count;
}

/**
 */
static inline uint32_t ringb_free_count(const struct ringb *r)
{
	return r->capacity - ringb_count(r);
}

/**
 */
static inline void ringb_reset(struct ringb *r)
{
	r->prod.head = r->cons.head = 0;
	r->prod.tail = r->cons.tail = 0;
}

static inline int ringb_full(const struct ringb *r)
{
	return ringb_free_count(r) == 0;
}

static inline int ringb_empty(const struct ringb *r)
{
	return ringb_count(r) == 0;
}

static inline uint32_t ringb_size(const struct ringb *r)
{
	return r->size;
}

static inline uint32_t ringb_capacity(const struct ringb *r)
{
	return r->capacity;
}

/**
 * 入队列
 */
extern uint32_t __ringb_do_enqueue(struct ringb *r, void * const *obj_table,
		uint32_t n, uint32_t behavior, uint32_t is_sp, uint32_t *free_space);

/**
 * 出队列
 */
extern uint32_t __ringb_do_dequeue(struct ringb *r, void **obj_table,
		 uint32_t n, uint32_t behavior, uint32_t is_sc, uint32_t *available);

/**
 * 批量入队列，空间不足返回0，否则返回 n
 * 多线程安全
 */
static inline
uint32_t ringb_mp_enqueue_bulk(struct ringb *r, void * const *obj_table,
		uint32_t n, uint32_t *free_space)
{
	return __ringb_do_enqueue(r, obj_table, n, RINGB_Q_FIXED, RINGB_M_MP,
				free_space);
}

/**
 * 批量入队列，空间不足返回0，否则返回 n
 * 单线程安全
 */
static inline
uint32_t ringb_sp_enqueue_bulk(struct ringb *r, void * const *obj_table,
		uint32_t n, uint32_t *free_space)
{
	return __ringb_do_enqueue(r, obj_table, n, RINGB_Q_FIXED, RINGB_M_SP,
				free_space);
}

/**
 * 批量入队列，空间不足返回0，否则返回 n
 * 是否多线程安全，依赖创建时的标志
 */
static inline
uint32_t ringb_enqueue_bulk(struct ringb *r, void * const *obj_table,
		uint32_t n, uint32_t *free_space)
{
	return __ringb_do_enqueue(r, obj_table, n, RINGB_Q_FIXED, r->prod.single,
				free_space);
}

/**
 * 入队列，空间不足返回0，否则返回 1
 * 单线程安全
 */
static inline uint32_t ringb_mp_enqueue(struct ringb *r, void *obj)
{
	return ringb_mp_enqueue_bulk(r, &obj, 1, NULL);
}

/**
 * 入队列，空间不足返回0，否则返回 1
 * 单线程安全
 */
static inline uint32_t ringb_sp_enqueue(struct ringb *r, void *obj)
{
	return ringb_sp_enqueue_bulk(r, &obj, 1, NULL);
}

/**
 * 入队列，空间不足返回0，否则返回 1
 * 是否多线程安全，依赖创建时的标志
 */
static inline uint32_t ringb_enqueue(struct ringb *r, void *obj)
{
	return ringb_enqueue_bulk(r, &obj, 1, NULL);
}

/**
 * 批量出队列，对象数不足返回 0，否则返回 n
 * 多线程安全
 */
static inline
uint32_t ringb_mc_dequeue_bulk(struct ringb *r, void **obj_table,
		uint32_t n, uint32_t *available)
{
	return __ringb_do_dequeue(r, obj_table, n, RINGB_Q_FIXED, RINGB_M_MC,
				available);
}

/**
 * 批量出队列，对象数不足返回 0，否则返回 n
 * 单线程安全
 */
static inline
uint32_t ringb_sc_dequeue_bulk(struct ringb *r, void **obj_table,
		uint32_t n, uint32_t *available)
{
	return __ringb_do_dequeue(r, obj_table, n, RINGB_Q_FIXED, RINGB_M_SC,
				available);
}

/**
 * 批量出队列，对象数不足返回 0，否则返回 n
 * 是否多线程安全，依赖创建时的标志
 */
static inline
uint32_t ringb_dequeue_bulk(struct ringb *r, void **obj_table, uint32_t n,
		uint32_t *available)
{
	return __ringb_do_dequeue(r, obj_table, n, RINGB_Q_FIXED, r->cons.single,
				available);
}

/**
 * 出队列，无对象返回0，否则返回 1
 * 多线程安全
 */
static inline uint32_t ringb_mc_dequeue(struct ringb *r, void **obj_p)
{
	return ringb_mc_dequeue_bulk(r, obj_p, 1, NULL);
}

/**
 * 出队列，无对象返回0，否则返回 1
 * 单线程安全
 */
static inline uint32_t ringb_sc_dequeue(struct ringb *r, void **obj_p)
{
	return ringb_sc_dequeue_bulk(r, obj_p, 1, NULL);
}

/**
 * 出队列，无对象返回0，否则返回 1
 * 是否多线程安全，依赖创建时的标志
 */
static inline uint32_t ringb_dequeue(struct ringb *r, void **obj_p)
{
	return ringb_dequeue_bulk(r, obj_p, 1, NULL);
}

/**
 * 尽可能的批量入队列，返回入队列的对象个数
 * 多线程安全
 */
static inline
uint32_t ringb_mp_enqueue_burst(struct ringb *r, void * const *obj_table,
		uint32_t n, uint32_t *free_space)
{
	return __ringb_do_enqueue(r, obj_table, n, RINGB_Q_VARIABLE, RINGB_M_MP,
				free_space);
}

/**
 * 尽可能的批量入队列，返回入队列的对象个数
 * 单线程安全
 */
static inline
uint32_t ringb_sp_enqueue_burst(struct ringb *r, void * const *obj_table,
		uint32_t n, uint32_t *free_space)
{
	return __ringb_do_enqueue(r, obj_table, n, RINGB_Q_VARIABLE, RINGB_M_SP,
				free_space);
}

/**
 * 尽可能的批量入队列，返回入队列的对象个数
 * 是否多线程安全，依赖创建时的标志
 */
static inline
uint32_t ringb_enqueue_burst(struct ringb *r, void * const *obj_table,
		uint32_t n, uint32_t *free_space)
{
	return __ringb_do_enqueue(r, obj_table, n, RINGB_Q_VARIABLE, r->prod.single,
				free_space);
}

/**
 * 尽可能的批量出队列，返回出队列的对象个数
 * 多线程安全
 */
static inline
uint32_t ringb_mc_dequeue_burst(struct ringb *r, void **obj_table,
		uint32_t n, uint32_t *available)
{
	return __ringb_do_dequeue(r, obj_table, n, RINGB_Q_VARIABLE, RINGB_M_MC,
				available);
}

/**
 * 尽可能的批量出队列，返回出队列的对象个数
 * 单线程安全
 */
static inline
uint32_t ringb_sc_dequeue_burst(struct ringb *r, void **obj_table,
		uint32_t n, uint32_t *available)
{
	return __ringb_do_dequeue(r, obj_table, n, RINGB_Q_VARIABLE, RINGB_M_SC,
				available);
}

/**
 * 尽可能的批量出队列，返回出队列的对象个数
 * 是否多线程安全，依赖创建时的标志
 */
static inline
uint32_t ringb_dequeue_burst(struct ringb *r, void **obj_table,
		uint32_t n, uint32_t *available)
{
	return __ringb_do_dequeue(r, obj_table, n, RINGB_Q_VARIABLE, r->cons.single,
				available);
}

__END_DECLS


#endif
