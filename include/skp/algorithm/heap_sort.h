#ifndef __us_heap_sort_h__
#define __us_heap_sort_h__

#include "../utils/compiler.h"

__BEGIN_DECLS

/**
 * heap_sort - sort an array of elements
 * @base: pointer to data to sort
 * @num: number of elements
 * @size: size of each element
 * @cmp_func: pointer to comparison function
 * @swap_func: pointer to swap function or NULL
 *
 * This function does a heapsort on the given array. You may provide a
 * swap_func function optimized to your element type.
 *
 * Sorting time is O(n log n) both on average and worst-case. While
 * qsort is about 20% faster on average, it suffers from exploitable
 * O(n*n) worst-case behavior and extra memory requirements that make
 * it less suitable for kernel use.
 */

extern void heap_sort(void *base, size_t num, size_t size,
    int (*cmp)(const void *, const void *), void (*swap)(void *, void *, int));

__END_DECLS


#endif