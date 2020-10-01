#ifndef __US_LIST_SORT_H__
#define __US_LIST_SORT_H__

#include "../utils/compiler.h"

__BEGIN_DECLS

struct list_head;
/**
 * list_sort - sort a list
 * @priv: private data, opaque to list_sort(), passed to @cmp
 * @head: the list to sort
 * @cmp: the elements comparison function
 *
 * This function implements "merge sort", which has O(nlog(n))
 * complexity.
 *
 * The comparison function @cmp must return a negative value if @a
 * should sort before @b, and a positive value if @a should sort after
 * @b. If @a and @b are equivalent, and their original relative
 * ordering is to be preserved, @cmp must return 0.
 */
extern void list_sort(void *priv, struct list_head *head,
		int (*cmp)(void *priv, struct list_head *a, struct list_head *b));

__END_DECLS

#endif