#ifndef __US_VECTOR_H__
#define __US_VECTOR_H__

#include "../utils/utils.h"

__BEGIN_DECLS

struct vector;
struct vector_ops {
	int (*compare_elem)(const char *elem, const char *key);
};
/*
 * 注意：vector是使用连续多个char内存来存值，
 * 所以存储指针时，应该把指针当做值，那么应该
 * 将指针的指针作为参数
 */
struct vector {
	char *start;
	char *end;
	uint32_t nr_elems;
	uint32_t elem_size;
	void *user_data;
	const struct vector_ops *ops;
};

#define DEFINE_VECTOR(name, esize, _ops)	\
	struct vector name = {			\
		.start = NULL, .end = NULL,	\
		.nr_elems = 0, .elem_size = esize, \
		.user_data = NULL, .ops = (_ops), \
	}


typedef void (*vector_fn)(char *elem, void* user);

extern void vector_init(struct vector *vec,
	size_t elem_size, const struct vector_ops *ops);

extern void vector_release(struct vector *, vector_fn action_fn, void *user);

/*保证元素值唯一性*/
extern char *
__vector_insert(struct vector *, const char *key, const char *elem);

extern int 
__vector_remove(struct vector *, const char *key, char *src_elem);

#define vector_insert(vec, key) \
	__vector_insert((vec), (key), (key))

#define vector_remove(vec, key) \
	__vector_remove((vec), (key), 0)

#define vector_for_each_elem(ptr, vec) \
	for ((ptr) = (vec)->start; (ptr) < (vec)->start + \
			(vec)->nr_elems * (vec)->elem_size;	\
			(ptr) += (vec)->elem_size)

static inline char *vector_idx(struct vector *vec, size_t idx)
{
	if (skp_unlikely(idx > vec->nr_elems))
		return NULL;
	return vec->start + vec->elem_size * idx;
}

static inline size_t vector_size(struct vector *vec)
{
	return vec->elem_size * vec->nr_elems;
}

static inline uint32_t vector_elems(struct vector *vec)
{
	return vec->nr_elems;
}

extern int __vector_copy(struct vector *dest,
	const struct vector *src, vector_fn action_fn, void *user);

#define vector_copy(dst, src) \
	__vector_copy((dst), (src), 0, 0)

__END_DECLS

#endif
