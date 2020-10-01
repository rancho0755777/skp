#include <skp/adt/vector.h>
#include <skp/mm/slab.h>

#ifdef DEBUG
# define VEC_NR_ELEMS (16)
#else
# define VEC_NR_ELEMS (128)
#endif

static inline int __expand(struct vector *vec, ssize_t *offset)
{
	char *ptr;
	size_t vec_size, new_size;

	*offset = 0;
	vec_size = vec->end - vec->start;
	if (skp_likely(vec->nr_elems * vec->elem_size < vec_size))
		return 0;

	new_size = vec_size + VEC_NR_ELEMS * vec->elem_size;

	ptr = malloc(new_size);
	if (skp_unlikely(!ptr))
		return -ENOMEM;

	if (skp_likely(vec_size)) {
		memcpy(ptr, vec->start, vec_size);
		free(vec->start);
	}

	*offset = ptr - vec->start;
	vec->start = ptr;
	vec->end = ptr + new_size;

	log_debug("expand success : %p [%zu] --> [%zu], %u",
		vec, vec_size/vec->elem_size, new_size/vec->elem_size, vec->elem_size);
	return 0;
}

static inline void __copy_elem(char *dst, const char *src, size_t size)
{
	switch(size) {
	case 1:
		*dst= *src;
		break;
	case 2:
		*((uint16_t*)dst) = *((uint16_t*)src);
		break;
	case 4:
		*((uint32_t*)dst) = *((uint32_t*)src);
		break;
	case 8:
		*((uint64_t*)dst) = *((uint64_t*)src);
		break;
	default:
		memcpy(dst, src, size);
	}
}

/*保证元素值唯一性*/
char *__vector_insert(struct vector *vec, const char *key, const char *elem)
{
	char *ptr;
	ssize_t offset;

	vector_for_each_elem(ptr, vec) {
		if (!vec->ops->compare_elem(ptr, key))
			return ptr;
	}

	if (__expand(vec, &offset))
		return ERR_PTR(-ENOMEM);

	if (skp_unlikely(!ptr)) {
		ptr = vec->start;
	} else {
		ptr += offset;
	}
	__copy_elem(ptr, elem, vec->elem_size);
	vec->nr_elems++;

	return NULL;
}

int __vector_remove(struct vector *vec, const char *key, char *src_elem)
{
	size_t size;
	char *ptr;

	vector_for_each_elem(ptr, vec) {
		if (!vec->ops->compare_elem(ptr, key))
			goto found;
	}
	return -ENOENT;
found:
	if (src_elem)
		__copy_elem(src_elem, ptr, vec->elem_size);
	size = vec->start + vec->nr_elems * vec->elem_size - (ptr + vec->elem_size);
	if (skp_likely(size))
		memmove(ptr, ptr + vec->elem_size, size);
	vec->nr_elems--;
	return 0;
}

void vector_init(struct vector *vec,
		size_t elem_size, const struct vector_ops *ops)
{
	BUG_ON(!elem_size);
	BUG_ON(elem_size >= U32_MAX);
	BUG_ON(!ops->compare_elem);
	WARN_ON(elem_size > 128);
	vec->start = NULL;
	vec->end = NULL;
	vec->nr_elems = 0;
	vec->user_data = NULL;
	vec->elem_size = (uint32_t)elem_size;
	vec->ops = ops;
}

void vector_release(struct vector *vec, vector_fn action_fn, void *user)
{
	if (action_fn) {
		char *ptr;
		vector_for_each_elem(ptr, vec)
			action_fn(ptr, user);
	}
	if (vec->start)
		free(vec->start);

	vec->start = NULL;
	vec->end = NULL;
	vec->nr_elems = 0;
}

int __vector_copy(struct vector *dst, const struct vector *src,
		vector_fn action_fn, void *user)
{
	char *start, *ptr;
	size_t size = src->nr_elems * src->elem_size;

	WARN_ON(dst->start);
	if (skp_unlikely(!size))
		return 0;
	start = malloc(size);
	if (skp_unlikely(!start))
		return -ENOMEM;

	dst->start = start;
	dst->end = start + size;
	dst->nr_elems = src->nr_elems;
	dst->elem_size = src->elem_size;

	vector_for_each_elem(ptr, src) {
		if (action_fn)
			action_fn(ptr, user);
		__copy_elem(start, ptr, src->elem_size);
		start += src->elem_size;
	}

	return 0;
}
