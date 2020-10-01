#include <skp/utils/utils.h>

#define FASTCALL(x) x __attribute__((regparm(3)))
#define fastcall __attribute__((regparm(3)))

static uint32_t SEQ = (1U << 23);

#define TEST_START() 			\
do {							\
	uint64_t _start, _end;		\
	volatile long X = 0;	\
	_start = abstime(0, 0); \
	for (volatile uint32_t i = 0; i < SEQ; i++) { \

#define TEST_END(name) 			\
	}							\
	_end = abstime(0, 0);	\
	printf("[%s] cost : %.3lf ns\n", name, (double)(_end - _start) / SEQ); \
} while (0)

static uint32_t FASTCALL(fast_func(const void *key, ssize_t keyLen));

static uint32_t fastcall fast_func(const void *key, ssize_t keyLen)
{
	/* 'm' and 'r' are mixing constants generated offline.
	 *   They're not really 'magic', they just happen to work well.  */
	const uint8_t * data = (void*)0;
	const uint32_t  m = 0x5bd1e995;
	const int		r = 24;
	const uint32_t	seed = 0x1507U;
	uint64_t l = 0;
	uint64_t hv = 0;

	if (skp_unlikely(!key)) {
		return 0;
	}

	if (keyLen < 0) {
		l = strlen((const char *)key);
	} else {
		l = keyLen;
	}

	/* Initialize the hash to a 'random' value */
	hv = seed ^ l;

	/* Mix 4 bytes at a time into the hash */
	data = (const uint8_t *)key;

	while (l >= 4) {
		uint32_t k = *(uint32_t *)data;

		k *= m;
		k ^= k >> r;
		k *= m;

		hv *= m;
		hv ^= k;

		data += 4;
		l -= 4;
	}

	/* Handle the last few bytes of the input array  */
	switch (l) {
		case 3: hv ^= data[2] << 16;
		case 2: hv ^= data[1] << 8;
		case 1: hv ^= data[0]; hv *= m;
	}

	/* Do a few final mixes of the hash to ensure the last few
	 * bytes are well-incorporated. */
	hv ^= hv >> 13;
	hv *= m;
	hv ^= hv >> 15;

	return (uint32_t)hv;
}

static int std_func(const void *key, ssize_t keyLen)
{
	/* 'm' and 'r' are mixing constants generated offline.
	 *   They're not really 'magic', they just happen to work well.  */
	const uint8_t * data = (void*)0;
	const uint32_t  m = 0x5bd1e995;
	const int		r = 24;
	const uint32_t	seed = 0x1507U;
	uint64_t l = 0;
	uint64_t hv = 0;

	if (skp_unlikely(!key)) {
		return 0;
	}

	if (keyLen < 0) {
		l = strlen((const char *)key);
	} else {
		l = keyLen;
	}

	/* Initialize the hash to a 'random' value */
	hv = seed ^ l;

	/* Mix 4 bytes at a time into the hash */
	data = (const uint8_t *)key;

	while (l >= 4) {
		uint32_t k = *(uint32_t *)data;

		k *= m;
		k ^= k >> r;
		k *= m;

		hv *= m;
		hv ^= k;

		data += 4;
		l -= 4;
	}

	/* Handle the last few bytes of the input array  */
	switch (l) {
		case 3: hv ^= data[2] << 16;
		case 2: hv ^= data[1] << 8;
		case 1: hv ^= data[0]; hv *= m;
	}

	/* Do a few final mixes of the hash to ensure the last few
	 * bytes are well-incorporated. */
	hv ^= hv >> 13;
	hv *= m;
	hv ^= hv >> 15;

	return (uint32_t)hv;
}

int main(int argc, char **argv)
{
#define TEXT_SIZE 512
	char *buff = malloc(TEXT_SIZE);
	BUG_ON(!buff);

	for (size_t i = 0; i < TEXT_SIZE; i++) {
		buff[i] = (char)prandom_int('a', 'z');	
	}

	TEST_START()
		X += std_func(buff, 1024);	
	TEST_END("stdcall");

	TEST_START()
		X += MurmurHash2(buff, 1024);	
	TEST_END("inline");

	TEST_START()
		X += fast_func(buff, 1024);	
	TEST_END("fastcall");

	return 0;
}
