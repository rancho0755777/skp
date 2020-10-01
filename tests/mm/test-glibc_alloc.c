//
//  glibc_alloc_test.c
//
//  Created by 周凯 on 2019/2/28.
//  Copyright © 2019 zhoukai. All rights reserved.
//

#include <skp/utils/utils.h>
#include <skp/adt/list.h>
#include <skp/mm/pgalloc.h>
#include <skp/mm/slab.h>

//#define JEMALLOC_MANGLE
//#define TCMALLOC_MANGLE
//#define SMALLBLK
//#define UMALLOC_MANGLE

#ifdef JEMALLOC_MANGLE
# include <jemalloc/jemalloc.h>
#elif defined UMALLOC_MANGLE
# define malloc(s) umalloc((s))
# define free(p) ufree((p))
#elif defined TCMALLOC_MANGLE
# include <gperftools/tcmalloc.h>
# define malloc(s) tc_malloc((s))
# define free(p) tc_free((p))
#endif

#ifdef SMALLBLK
# undef VPAGE_SIZE
# define VPAGE_SIZE sizeof(struct vpage)
#endif

int main(int argc, char **argv)
{
	LIST__HEAD(list);
	int order = 0;
	int count = 20;

	struct vpage *page;
	uint64_t start, end;

	if (argc > 1)
		order = atoi(argv[1]);
	if (argc > 2)
		count = atoi(argv[2]);

	order = clamp(order, 0, MAX_ORDER - 1);
	count = clamp(count, 0, 20);

	count = 1 << count;

#if defined JEMALLOC_MANGLE || defined UMALLOC_MANGLE
	log_info("test param : block %ld Bytes, times %d",
		(VPAGE_SIZE << order), count);
#else
	log_info("test param : block %ld MBytes, times %d",
		(VPAGE_SIZE << order) >> 20, count);
#endif

	start = similar_abstime(0, 0);
	for (int i = 0; i < count; i++) {
		page = malloc(VPAGE_SIZE << order);
		if (WARN_ON(!page))
			break;
		add_page_to_list_tail(page, &list);
	}
	end = similar_abstime(0, 0);

	log_info("alloc spend %.2lf", ((double)(end - start)) / count);
	start = similar_abstime(0, 0);
	for (int i = 0; i < count; i++) {
		page = list_first_entry_or_null(&list, struct vpage, lru);
		if (WARN_ON(!page))
			break;
		del_page_from_list(page);
		free(page);
	}

	end = similar_abstime(0, 0);
	log_info("free spend %.2lf", ((double)(end - start)) / count);

	start = similar_abstime(0, 0);
	for (int i = 0; i < count; i++) {
		page = malloc(VPAGE_SIZE << order);
		if (WARN_ON(!page))
			break;
		add_page_to_list_tail(page, &list);
	}
	end = similar_abstime(0, 0);

	log_info("alloc spend %.2lf", ((double)(end - start)) / count);
	start = similar_abstime(0, 0);
	for (int i = 0; i < count; i++) {
		page = list_first_entry_or_null(&list, struct vpage, lru);
		if (WARN_ON(!page))
			break;
		del_page_from_list(page);
		free(page);
	}

	end = similar_abstime(0, 0);

	log_info("free spend %.2lf", ((double)(end - start)) / count);
	return EXIT_SUCCESS;
}
