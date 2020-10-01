//
//  mmsetup_test.c
//
//  Created by 周凯 on 2019/2/28.
//  Copyright © 2019 zhoukai. All rights reserved.
//

#include <skp/mm/pgalloc.h>

//#define WRITE_DATA

#ifdef WRITE_DATA
static void write_data(struct vpage *page)
{
	char *ptr = page_to_virt(page);
	*ptr = '1';
}
#else
# define write_data(p) ((void)(p))
#endif

int main(int argc, char **argv)
{
	LIST__HEAD(list);

	int rand = 0;
	int order = 0;
	int count = 22;
	struct vpage *page;
	uint64_t start, end;

	if (argc > 1)
		order = atoi(argv[1]);
	if (argc > 2)
		count = atoi(argv[2]);
	if (argc > 3)
		rand = __GFP_COMP;

	order = clamp(order, 0, MAX_ORDER - 1);
	count = clamp(count, 0, 30);

	count = 1 << count;

	log_info("test param : block %ld MBytes, times %d",
		(VPAGE_SIZE << order) >> 20, count);

	setup_memory();

	/*compound*/
	page = __alloc_pages(__GFP_COMP, 3);
	BUG_ON(!page);
	for (int i = 0; i < (1 << 3); i++) {
		struct vpage *curr = &page[i];
		BUG_ON(page != compound_head(curr));
		BUG_ON(3 != compound_order(curr));
	}

	for (int nr_test = 0; nr_test < 2; nr_test++) {
		start = similar_abstime(0, 0);
		for (int i = 0; i < count; i++) {
			page = __alloc_pages(rand, skp_unlikely(rand) ?
						(int)prandom_int(1, max(order, 6)) : order);
			if (WARN_ON(!page))
				break;
			write_data(page);
			add_page_to_list_tail(page, &list);
		}
		end = similar_abstime(0, 0);
		log_info("alloc spend %.2lf", ((double)(end - start)) / count);

		start = similar_abstime(0, 0);
		for (int i = 0; i < count; i++) {
			page = first_page_on_list(&list);
			if (WARN_ON(!page))
				break;
			del_page_from_list(page);
			__free_pages(page, skp_unlikely(rand) ? compound_order(page) : order);
		}

		end = similar_abstime(0, 0);
		log_info("free spend %.2lf", ((double)(end - start)) / count);
	}

	pgcache_reclaim();

	return EXIT_SUCCESS;
}
