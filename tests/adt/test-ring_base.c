#include <skp/adt/ring.h>

#define TEST_CP (127)

int main(void)
{
	uint32_t rn;
	uintptr_t ptr;
	uintptr_t value[4] = { 0, 1, 2, 3 };

	struct ringb *r = ringb_create(TEST_CP, 0);
	BUG_ON(r);

	r = ringb_create(TEST_CP, RINGB_F_EXACT_SZ|RINGB_F_SP_ENQ|RINGB_F_SC_DEQ);
	BUG_ON(!r);

	rn = ringb_dequeue(r, NULL);
	BUG_ON(rn);

	for (int i = 0; i < TEST_CP; i++) {
		rn = ringb_enqueue(r, (uintptr_t)i);
		BUG_ON(!rn);
	}

	rn = ringb_enqueue(r, 0);
	BUG_ON(rn);

	for (int i = 0; i < TEST_CP; i++) {
		rn = ringb_dequeue(r, (void**)&ptr);
		BUG_ON(!rn);
		BUG_ON(ptr!=i);
	}

	for (int i=0; i<TEST_CP/ARRAY_SIZE(value); i++) {
		rn=ringb_enqueue_bulk(r, (void**)value, ARRAY_SIZE(value), NULL);
		BUG_ON(rn!=ARRAY_SIZE(value));
	}

	rn=ringb_enqueue_bulk(r, (void**)value, ARRAY_SIZE(value), NULL);
	BUG_ON(rn!=0);

	rn=ringb_enqueue_burst(r, (void**)value, ARRAY_SIZE(value), NULL);
	BUG_ON(rn!=TEST_CP%ARRAY_SIZE(value));

	for (int i=0; i<TEST_CP/ARRAY_SIZE(value); i++) {
		memset(value, 0, sizeof(value));
		rn=ringb_dequeue_bulk(r, (void**)value, ARRAY_SIZE(value), NULL);
		BUG_ON(rn!=ARRAY_SIZE(value));
		for (int j=0; j<ARRAY_SIZE(value); j++) {
			BUG_ON(value[j]!=j);
		}
	}

	rn=ringb_dequeue_bulk(r, (void**)value, ARRAY_SIZE(value), NULL);
	BUG_ON(rn!=0);

	rn=ringb_dequeue_burst(r, (void**)value, ARRAY_SIZE(value), NULL);
	BUG_ON(rn!=TEST_CP%ARRAY_SIZE(value));
	for (int j=0; j<TEST_CP%ARRAY_SIZE(value); j++) {
		BUG_ON(value[j]!=j);
	}

	ringb_free(r);

	return EXIT_SUCCESS;
}
