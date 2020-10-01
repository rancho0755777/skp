#define pr_fmt(fmt) "list_sort_test: " fmt

#include <skp/adt/list.h>
#include <skp/utils/utils.h>
#include <skp/algorithm/list_sort.h>

/*
 * The pattern of set bits in the list length determines which cases
 * are hit in list_sort().
 */
#define TEST_LIST_LEN (512+128+2) /* not including head */

#define TEST_POISON1 0xDEADBEEF
#define TEST_POISON2 0xA324354C

struct debug_el {
	unsigned int poison1;
	struct list_head list;
	unsigned int poison2;
	int value;
	unsigned serial;
};

/* Array, containing pointers to all elements in the test list */
static struct debug_el **elts;

static int check(struct debug_el *ela, struct debug_el *elb)
{
	if (ela->serial >= TEST_LIST_LEN) {
		log_error("error: incorrect serial %d", ela->serial);
		return -EINVAL;
	}
	if (elb->serial >= TEST_LIST_LEN) {
		log_error("error: incorrect serial %d", elb->serial);
		return -EINVAL;
	}
	if (elts[ela->serial] != ela || elts[elb->serial] != elb) {
		log_error("error: phantom element");
		return -EINVAL;
	}
	if (ela->poison1 != TEST_POISON1 || ela->poison2 != TEST_POISON2) {
		log_error("error: bad poison: %#x/%#x",
			ela->poison1, ela->poison2);
		return -EINVAL;
	}
	if (elb->poison1 != TEST_POISON1 || elb->poison2 != TEST_POISON2) {
		log_error("error: bad poison: %#x/%#x",
			elb->poison1, elb->poison2);
		return -EINVAL;
	}
	return 0;
}

static int cmp(void *priv, struct list_head *a, struct list_head *b)
{
	struct debug_el *ela, *elb;

	ela = container_of(a, struct debug_el, list);
	elb = container_of(b, struct debug_el, list);

	check(ela, elb);
	return ela->value - elb->value;
}

int main(void)
{
	int i, count = 1, err = -ENOMEM;
	struct debug_el *el;
	struct list_head *cur;
	LIST__HEAD(head);

	log_debug("start testing list_sort()");

	elts = calloc(TEST_LIST_LEN, sizeof(*elts));
	if (!elts)
		return err;

	for (i = 0; i < TEST_LIST_LEN; i++) {
		el = malloc(sizeof(*el));
		BUG_ON(!el);

		 /* force some equivalencies */
		el->value = prandom() % (TEST_LIST_LEN / 3);
		el->serial = i;
		el->poison1 = TEST_POISON1;
		el->poison2 = TEST_POISON2;
		elts[i] = el;
		list_add_tail(&el->list, &head);
	}

	cycles_t start = get_cycles();
	list_sort(NULL, &head, cmp);
	cycles_t end = get_cycles();
	log_info("sort cast [%d] : %lld cycles, %lld ns", TEST_LIST_LEN,
		end-start, cycles_to_ns(end-start));

	err = -EINVAL;
	for (cur = head.next; cur->next != &head; cur = cur->next) {
		struct debug_el *el1;
		int cmp_result;

		if (cur->next->prev != cur) {
			log_error("error: list is corrupted");
			BUG();
		}

		cmp_result = cmp(NULL, cur, cur->next);
		if (cmp_result > 0) {
			log_error("error: list is not sorted");
			BUG();
		}

		el = container_of(cur, struct debug_el, list);
		el1 = container_of(cur->next, struct debug_el, list);
		if (cmp_result == 0 && el->serial >= el1->serial) {
			log_error("error: order of equivalent elements not "
				"preserved");
			BUG();
		}

		if (check(el, el1)) {
			log_error("error: element check failed");
			BUG();
		}
		count++;
	}
	if (head.prev != cur) {
		log_error("error: list is corrupted");
		BUG();
	}

	if (count != TEST_LIST_LEN) {
		log_error("error: bad list length %d", count);
		BUG();
	}

	log_info("test passed");

	err = 0;
	for (i = 0; i < TEST_LIST_LEN; i++)
		free(elts[i]);
	free(elts);
	return err;
}