#include <skp/utils/utils.h>
#include <skp/utils/hash.h>

#define HASH_BITS 6
#define NR_HASH (1U << 30)

static uint32_t distribution[1 << HASH_BITS] = { };

int main(int argc, char const *argv[])
{
	uint64_t start, end;
	uint32_t *calc_value;

	start = abstime(0, 0);

	for (int i = 0; i < NR_HASH; i++) {
		//volatile uint32_t value = hash_32(i, HASH_BITS);
		//volatile uint32_t value = __hash_32(i) & ((1U<<HASH_BITS) - 1);
		volatile uint32_t value = __hash_32(i);
		value = (value >> 16 | value << 16);
		value &= ((1U<<HASH_BITS) - 1);
		BUG_ON(value >= ARRAY_SIZE(distribution));
		distribution[value]++;
	}

	end = abstime(0, 0);

	printf("cost : %ld\n", (end - start) / (1 << 30));

	for (int i = 0; i < ARRAY_SIZE(distribution); i++) {
		printf(" > %03d : %8d\n", i, distribution[i]);
	}
	printf("---------------------------------------------------\n");

	BUG_ON(!(calc_value = malloc(sizeof(*calc_value) * NR_HASH)));

	for (int i = 0; i < NR_HASH; i++) {
		calc_value[i] = prandom_int(1, NR_HASH);
	}

	start = abstime(0, 0);

	for (int i = 0; i < NR_HASH; i++) {
		//volatile uint32_t value = hash_32(calc_value[i], HASH_BITS);
		//volatile uint32_t value = __hash_32(calc_value[i]) & ((1U<<HASH_BITS) - 1);
		volatile uint32_t value = __hash_32(calc_value[i]);
		value = (value >> 16 | value << 16);
		value &= ((1U<<HASH_BITS) - 1);
		BUG_ON(value >= ARRAY_SIZE(distribution));
		distribution[value]++;
	}

	end = abstime(0, 0);

	printf("cost : %ld\n", (end - start) / NR_HASH);

	for (int i = 0; i < ARRAY_SIZE(distribution); i++) {
		printf(" > %03d : %8d\n", i, distribution[i]);
	}
	return 0;
}
