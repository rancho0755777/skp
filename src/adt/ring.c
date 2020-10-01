#include <skp/adt/ring.h>

#include <skp/mm/slab.h>

#define RINGB_SZ_MASK  (0x7fffffffU)

ssize_t ringb_calc_memsize(uint32_t count, uint32_t flags)
{
	ssize_t sz;

	if (WARN_ON(flags & ~(RINGB_F_SC_DEQ|RINGB_F_SP_ENQ|RINGB_F_EXACT_SZ)))
		return -EINVAL;

	if (flags & RINGB_F_EXACT_SZ) {
		count = roundup_pow_of_two(count + 1);
	} else if (skp_likely(!is_power_of_2(count))) {
		log_error("Requested size is invalid, must be power of 2, and "
			"do not exceed the size limit %u\n", RINGB_SZ_MASK);
		return -EINVAL;
	}

 	if (skp_unlikely(count > RINGB_SZ_MASK)) {
		log_error("Requested size is invalid, must be power of 2, and "
				  "do not exceed the size limit %u\n", RINGB_SZ_MASK);
		return -EINVAL;
	}

	sz = sizeof(struct ringb) + count * sizeof(void *);
	sz = ALIGN(sz, CACHELINE_BYTES);
	return sz;
}

int ringb_init(struct ringb *r, uint32_t count, uint32_t flags)
{
	/* compilation-time checks */
	BUILD_BUG_ON((sizeof(struct ringb) % CACHELINE_BYTES));
	BUILD_BUG_ON((offsetof(struct ringb, cons) % CACHELINE_BYTES));
	BUILD_BUG_ON((offsetof(struct ringb, prod) % CACHELINE_BYTES));

	if (WARN_ON(flags & ~(RINGB_F_SC_DEQ|RINGB_F_SP_ENQ|RINGB_F_EXACT_SZ)))
		return -EINVAL;

	/* init the ring structure */
	memset(r, 0, sizeof(*r));

	r->flags = flags;
	r->user = NULL;
	r->prod.single = (flags & RINGB_F_SP_ENQ) ? RINGB_M_SP : RINGB_M_MP;
	r->cons.single = (flags & RINGB_F_SC_DEQ) ? RINGB_M_SC : RINGB_M_MC;

	if (flags & RINGB_F_EXACT_SZ) {
		r->size = roundup_pow_of_two(count + 1);
		r->mask = r->size - 1;
		r->capacity = count;
	} else {
		if ((!is_power_of_2(count)) || (count > RINGB_SZ_MASK )) {
			log_error("Requested size is invalid, must be power of 2, and "
					  "do not exceed the size limit %u\n", RINGB_SZ_MASK);
			return -EINVAL;
		}
		r->size = count;
		r->mask = count - 1;
		r->capacity = r->mask;
	}
	r->prod.head = r->cons.head = 0;
	r->prod.tail = r->cons.tail = 0;

	return 0;
}

struct ringb *ringb_create(uint32_t count, uint32_t flags)
{
	ssize_t size = ringb_calc_memsize(count, flags);
	if (skp_unlikely(size<0)) {
		errno = EINVAL;
		return NULL;
	}

	struct ringb *r = aligned_alloc(__alignof__(*r), size);
	if (skp_unlikely(!r)) {
		errno = ENOMEM;
		return NULL;
	}

	ringb_init(r, count, flags);
	return r;
}

void ringb_free(struct ringb *r)
{
	if (skp_likely(r))
		free(r);
}

static __always_inline void
__ringb_update_tail(struct ringb_headtail *ht, uint32_t old_val, uint32_t new_val,
		uint32_t single, uint32_t enqueue)
{
	/*X86平台只需要静态 内存栅栏*/
#if 0
	if (enqueue) {
		smp_wmb();
	} else {
		smp_rmb();
	}
#else
	(void)enqueue;
	static_mb();
#endif

	/*
	 * If there are other enqueues/dequeues in progress that preceded us,
	 * we need to wait for them to complete
	 */
	if (!single)
		while (skp_unlikely(READ_ONCE(ht->tail) != old_val))
			cpu_relax();

	WRITE_ONCE(ht->tail, new_val);
}

/**
 * @internal 更新生产者队列头
 *
 * @param r
 *   A pointer to the ring structure
 * @param is_sp
 *   是否为单生产者
 * @param behavior
 *   RINGB_Q_FIXED:    排队固定数量的对象
 *   RINGB_Q_VARIABLE: Enqueue as many items as possible from ring
 * @return
 *   返回实际入队列的对象数量
 *   如果 behavior 为 RINGB_Q_FIXED, 空间不足返回0，并且不会改变队列状态
 */
static __always_inline
uint32_t __ringb_move_prod_head(struct ringb *r, uint32_t is_sp, uint32_t n,
		uint32_t behavior, uint32_t *old_head, uint32_t *new_head,
		uint32_t *free_entries)
{
	uint32_t max = n;
	const uint32_t capacity = r->capacity;

	do {
		/* Reset n to the initial burst count */
		n = max;

		*old_head = READ_ONCE(r->prod.head);
		/*
		 * add rmb barrier to avoid load/load reorder in weak
		 * memory model. It is noop on x86
		 */
#if 0
		smp_rmb();
#endif

		/*
		 * 两个无符号减法，即使回绕也不会又问题
		 */
		*free_entries = (capacity + READ_ONCE(r->cons.tail) - *old_head);

		/* check that we have enough room in ring */
		if (skp_unlikely(n > *free_entries))
			n = (behavior == RINGB_Q_FIXED) ? 0 : *free_entries;

		if (n == 0)
			return 0;

		*new_head = *old_head + n;
		if (is_sp) {
			WRITE_ONCE(r->prod.head, *new_head);
#if 0
			smp_rmb();
#endif
			break;
		}
	} while (skp_unlikely(!cmpxchg(&r->prod.head, *old_head, *new_head)));

	return n;
}

/**
 * @internal 更新消费者队列头
 *
 * @return
 *   实际出队列的对象数量
 *   如果 behavior 为 RINGB_Q_FIXED, 对象数目不足返回0，并且不会改变队列状态 *
 */
static __always_inline
uint32_t __ringb_move_cons_head(struct ringb *r, uint32_t is_sc, uint32_t n,
		uint32_t behavior, uint32_t *old_head, uint32_t *new_head,
		uint32_t *entries)
{
	uint32_t max = n;

	/* move cons.head atomically */
	do {
		/* Restore n as it may change every loop */
		n = max;

		*old_head = READ_ONCE(r->cons.head);

		/* add rmb barrier to avoid load/load reorder in weak
		 * memory model. It is noop on x86
		 */
#if 0
		smp_rmb();
#endif

		/*
		 * 两个无符号减法，即使回绕也不会又问题
		 */
		*entries = READ_ONCE(r->prod.tail) - *old_head;

		/* Set the actual entries for dequeue */
		if (n > *entries)
			n = (behavior == RINGB_Q_FIXED) ? 0 : *entries;

		if (skp_unlikely(n == 0))
			return 0;

		*new_head = *old_head + n;
		if (is_sc) {
			WRITE_ONCE(r->cons.head, *new_head);
#if 0
			smp_rmb();
#endif
			break;
		}
	} while (skp_unlikely(!cmpxchg(&r->cons.head, *old_head, *new_head)));

	return n;
}

static __always_inline
void ENQUEUE_PTRS(struct ringb *r, uint32_t prod_head, void * const *obj_table,
		uint32_t n)
{
	uint32_t i;
	void **ring = (void*)r->ring;
	const uint32_t size = (r)->size;
	uint32_t idx = prod_head & (r)->mask;

	if (skp_likely(idx + n < size)) {
		/*连续的情况下，4个为单位进行批量复制，减少循环次数*/
		for (i = 0; i < (n & ((~0x3U))); i+=4, idx+=4) {
			ring[idx] = obj_table[i];
			ring[idx+1] = obj_table[i+1];
			ring[idx+2] = obj_table[i+2];
			ring[idx+3] = obj_table[i+3];
		}
		switch (n & 0x3U) {
		case 3:
			ring[idx++] = obj_table[i++]; /* fallthrough */
		case 2:
			ring[idx++] = obj_table[i++]; /* fallthrough */
		case 1:
			ring[idx++] = obj_table[i++];
		}
	} else {
		for (i = 0; idx < size; i++, idx++)
			ring[idx] = obj_table[i];
		for (idx = 0; i < n; i++, idx++)
			ring[idx] = obj_table[i];
	}
}

uint32_t __ringb_do_enqueue(struct ringb *r, void * const *obj_table,
		uint32_t n, uint32_t behavior, uint32_t is_sp, uint32_t *free_space)
{
	uint32_t free_entries;
	uint32_t prod_head, prod_next;

	n = __ringb_move_prod_head(r, is_sp, n, behavior,
			&prod_head, &prod_next, &free_entries);
	if (n == 0)
		goto end;

	ENQUEUE_PTRS(r, prod_head, obj_table, n);

	__ringb_update_tail(&r->prod, prod_head, prod_next, is_sp, 1);
end:
	if (free_space)
		*free_space = free_entries - n;
	return n;
}

static __always_inline
void DEQUEUE_PTRS(struct ringb *r, uint32_t cons_head, void **obj_table,
		uint32_t n)
{
	uint32_t i;
	void **ring = (void**)r->ring;
	const uint32_t size = (r)->size;
	uint32_t idx = cons_head & (r)->mask;

	if (skp_likely(idx + n < size)) {
		for (i = 0; i < (n & (~0x3U)); i+=4, idx+=4) {
			obj_table[i] = ring[idx];
			obj_table[i+1] = ring[idx+1];
			obj_table[i+2] = ring[idx+2];
			obj_table[i+3] = ring[idx+3];
		}
		switch (n & 0x3U) {
		case 3:
			obj_table[i++] = ring[idx++];
		case 2:
			obj_table[i++] = ring[idx++];
		case 1:
			obj_table[i++] = ring[idx++];
		}
	} else {
		for (i = 0; idx < size; i++, idx++)
			obj_table[i] = ring[idx];
		for (idx = 0; i < n; i++, idx++)
			obj_table[i] = ring[idx];
	}
}

uint32_t __ringb_do_dequeue(struct ringb *r, void **obj_table,
		uint32_t n, uint32_t behavior, uint32_t is_sc, uint32_t *available)
{
	uint32_t entries;
	uint32_t cons_head, cons_next;

	n = __ringb_move_cons_head(r, is_sc, n, behavior,
			&cons_head, &cons_next, &entries);
	if (n == 0)
		goto end;

	DEQUEUE_PTRS(r, cons_head, obj_table, n);

	__ringb_update_tail(&r->cons, cons_head, cons_next, is_sc, 0);
end:
	if (available)
		*available = entries - n;
	return n;
}
