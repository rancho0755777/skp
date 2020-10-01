#include <skp/utils/utils.h>
#include <skp/algorithm/heap_sort.h>

#define TEST_LEN 1000

static int cmpint(const void *a, const void *b)
{
	return *(int *)a - *(int *)b;
}

int main(void)
{
	int *a, i, r = 1, err = -ENOMEM;

	a = calloc(TEST_LEN, sizeof(*a));
    BUG_ON(!a);

	for (i = 0; i < TEST_LEN; i++) {
		r = (r * 725861) % 6599;
		a[i] = r;
	}

	cycles_t start = get_cycles();
	heap_sort(a, TEST_LEN, sizeof(*a), cmpint, NULL);
    cycles_t end = get_cycles();
	log_info("sort cast [%d] : %lld cycles, %lld ns", TEST_LEN, end-start,
        cycles_to_ns(end-start));

	err = -EINVAL;
	for (i = 0; i < TEST_LEN-1; i++)
		if (a[i] > a[i+1]) {
			log_error("test has failed");
            BUG();
		}
	err = 0;
	log_info("test passed");

	free(a);
	return err;
}