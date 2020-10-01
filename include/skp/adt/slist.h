#ifndef __SU_SLIST_H__
#define __SU_SLIST_H__

#include "../utils/utils.h"

__BEGIN_DECLS

struct slist_node;
#define SLIST_POISON  ((struct slist_node *) 0x00100100)

struct slist_node {
	struct slist_node *next;
};

struct slist_head {
	struct slist_node *head;
	struct slist_node *tail;
};

#define SLIST_HEAD_INIT(name) { NULL, NULL }


#define SLIST__HEAD(name) \
	struct slist_head name = SLIST_HEAD_INIT(name)

static inline void INIT_SLIST_NODE(struct slist_node *node)
{
	node->next = SLIST_POISON;
}

static inline bool slist_node_inlist(struct slist_node *node)
{
	return !!(READ_ONCE(node->next) != SLIST_POISON);
}

static inline void INIT_SLIST_HEAD(struct slist_head *list)
{
	WRITE_ONCE(list->head, NULL);
	WRITE_ONCE(list->tail, NULL);
}

static inline bool slist_empty(struct slist_head *list)
{
	return !READ_ONCE(list->head);
}

static inline bool slist_empty_careful(struct slist_head *list)
{
	return !READ_ONCE(list->tail) && !READ_ONCE(list->head);
}

static inline void slist_add_tail(struct slist_head *list, struct slist_node *node)
{
	WRITE_ONCE(node->next, NULL);
	if (!slist_empty(list)) {
		WRITE_ONCE(list->tail->next, node);
		list->tail = node;
	} else {
		list->tail = list->head = node;
	}
}

static inline void slist_add_head(struct slist_head *list, struct slist_node *node)
{
	WRITE_ONCE(node->next, list->head);
	if (!slist_empty(list)) {
		list->head = node;
	} else {
		list->head = list->tail = node;
	}
}

static inline struct slist_node *slist_shift(struct slist_head *list)
{
	struct slist_node *node = READ_ONCE(list->head);

	if (node) {
		if (list->tail == node) {
			list->head = list->tail = NULL;
		} else {
			list->head = node->next;
		}
	}
	return node;
}

static inline struct slist_node *slist_shift_init(struct slist_head *list)
{
	struct slist_node *node = slist_shift(list);
	if (node) {
		INIT_SLIST_NODE(node);
	}
	return node;
}

static inline void slist_splice(struct slist_head *src, struct slist_head *dst)
{
	if (slist_empty(src))
		return;
	WRITE_ONCE(src->tail->next, dst->head);
	WRITE_ONCE(dst->head, src->head);
}

static inline void slist_splice_init(struct slist_head *src, struct slist_head *dst)
{
	slist_splice(src, dst);
	INIT_SLIST_HEAD(src);
}

static __always_inline void slist_splice_tail(
		struct slist_head *src, struct slist_head *dst)
{
	if (slist_empty(src))
		return;
	if (slist_empty(dst)) {
		dst->head = src->head;
		dst->tail = src->tail;
	} else {
		WRITE_ONCE(dst->tail->next, src->head);
		WRITE_ONCE(dst->tail, src->tail);
	}
}

static __always_inline void slist_splice_tail_init(
		struct slist_head *src, struct slist_head *dst)
{
	slist_splice_tail(src, dst);
	INIT_SLIST_HEAD(src);
}

#define slist_entry(ptr, type, member) container_of(ptr, type, member)

#define slist_entry_safe(ptr, type, member) \
({	typeof(ptr) ____ptr = (ptr); 		\
	____ptr ? slist_entry(____ptr, type, member) : NULL; \
})

#define slist_shift_entry(list, type, member) \
({	type * ____ptr = NULL;                      	\
	struct slist_node *node = slist_shift(list); 	\
	if (node)                                    	\
		____ptr = slist_entry(node, type, member);	\
	____ptr;										\
})

__END_DECLS

#endif
