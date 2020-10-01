#include <skp/utils/bitops.h>
#include <skp/utils/utils.h>

int main(int argc, char **argv)
{
	{
		uint8_t v8 = 0;
		uint16_t v16 = 0;
		uint32_t v32 = 0;
		uint64_t v64 = 0;

		v32 = ALIGN(768, 1024);
		BUG_ON(!IS_ALIGNED(v32, 1024));
		BUG_ON(v32 != 1024);
		v32 = ALIGN_DOWN(768, 512);
		BUG_ON(!IS_ALIGNED(v32, 512));
		BUG_ON(v32 != 512);

		v32 = DIV_ROUND_UP(768, 1024);
		BUG_ON(v32 != 1);
		BUILD_BUG_ON(GENMASK_ULL(39, 21) != 0x000000ffffe00000ULL);
		BUILD_BUG_ON(GENMASK_ULL(31, 0) != 0x00000000ffffffffULL);

		v8 = rol8(0xfe, 2);
		BUG_ON(v8 != 0xfb);
		v8 = ror8(0xfe, 2);
		BUG_ON(v8 != 0xbf);

		v16 = rol16(0xfffe, 2);
		BUG_ON(v16 != 0xfffb);
		v16 = ror16(0xfffe, 2);
		BUG_ON(v16 != 0xbfff);

		v32 = rol32(0xfffffffe, 2);
		BUG_ON(v32 != 0xfffffffb);
		v32 = ror32(0xfffffffe, 2);
		BUG_ON(v32 != 0xbfffffff);

		v64 = rol64(0xfffffffffffffffe, 2);
		BUG_ON(v64 != 0xfffffffffffffffb);
		v64 = ror64(0xfffffffffffffffe, 2);
		BUG_ON(v64 != 0xbfffffffffffffff);
	}

	{
		bool rc = 0;
		unsigned long bitmap[2] = { 0, 0, };

		set_bit(BITS_PER_LONG, bitmap);
		BUG_ON(bitmap[0]);
		BUG_ON(bitmap[1] != 1);
		BUG_ON(!test_bit(BITS_PER_LONG, bitmap));

		__set_bit(BITS_PER_LONG - 1, bitmap);
		BUG_ON(bitmap[0] != (1UL << (BITS_PER_LONG - 1)));
		BUG_ON(!bitmap[1]);
		BUG_ON(!test_bit(BITS_PER_LONG - 1, bitmap));

		clear_bit(BITS_PER_LONG, bitmap);
		BUG_ON(test_bit(BITS_PER_LONG, bitmap));
		__clear_bit(BITS_PER_LONG - 1, bitmap);
		BUG_ON(test_bit(BITS_PER_LONG - 1, bitmap));

		change_bit(BITS_PER_LONG, bitmap);
		BUG_ON(!test_bit(BITS_PER_LONG, bitmap));
		__change_bit(BITS_PER_LONG, bitmap);
		BUG_ON(test_bit(BITS_PER_LONG, bitmap));

		rc = test_and_set_bit(BITS_PER_LONG, bitmap);
		BUG_ON(rc);
		BUG_ON(!test_bit(BITS_PER_LONG, bitmap));
		rc = __test_and_set_bit(BITS_PER_LONG - 1, bitmap);
		BUG_ON(rc);
		BUG_ON(bitmap[0] != (1UL << (BITS_PER_LONG - 1)));

		rc = test_and_clear_bit(BITS_PER_LONG, bitmap);
		BUG_ON(!rc);
		BUG_ON(test_bit(BITS_PER_LONG, bitmap));
		rc = __test_and_clear_bit(BITS_PER_LONG - 1, bitmap);
		BUG_ON(!rc);
		BUG_ON(bitmap[0]);
		BUG_ON(bitmap[0] == (1UL << (BITS_PER_LONG - 1)));

		rc = test_and_change_bit(BITS_PER_LONG, bitmap);
		BUG_ON(rc);
		BUG_ON(!test_bit(BITS_PER_LONG, bitmap));
		rc = __test_and_change_bit(BITS_PER_LONG - 1, bitmap);
		BUG_ON(rc);
		BUG_ON(bitmap[0] != (1UL << (BITS_PER_LONG - 1)));
	}

	{
		int idx = 0;
		unsigned vu = 0x010fffffU;
		unsigned long vl = 0x020fffffUL;
		unsigned long long vll = 0x040fffffULL;

		idx = fls(16);
		BUG_ON(idx != 5);
		idx = fls(vu);
		BUG_ON(idx != 25);
		idx = flsl(vl);
		BUG_ON(idx != 26);
		idx = flsll(vll);
		BUG_ON(idx != 27);

		idx = __fls(vu);
		BUG_ON(idx != 24);
		idx = __flsl(vl);
		BUG_ON(idx != 25);
		idx = __flsll(vll);
		BUG_ON(idx != 26);


		vu = 0xffff0010U;
		vl = 0xffff0020UL;
		vll = 0xffff0040U;

		idx = ffs(vu);
		BUG_ON(idx != 5);
		idx = ffsl(vl);
		BUG_ON(idx != 6);
		idx = ffsll(vll);
		BUG_ON(idx != 7);

		idx = __ffs(vu);
		BUG_ON(idx != 4);
		idx = __ffsl(vl);
		BUG_ON(idx != 5);
		idx = __ffsll(vll);
		BUG_ON(idx != 6);
	}

	{
		int weight;
		uint8_t u8 = 0x7eU;
		uint16_t u16 = 0x7e7eU;
		uint32_t u32 = 0x7e7e7e7eU;
		uint64_t u64 = 0x7e7e7e7e7e7e7e7eU;

		weight = hweight8(u8);
		BUG_ON(weight != 6);
		weight = hweight16(u16);
		BUG_ON(weight != 12);
		weight = hweight32(u32);
		BUG_ON(weight != 24);
		weight = hweight64(u64);
		BUG_ON(weight != 48);
	}

	{
		int order;
		order = get_count_order(0);
		BUG_ON(order!=-1);
		order = get_count_order(7);
		BUG_ON(order!=3);
		order = get_count_order(8);
		BUG_ON(order!=3);

		order = ilog2(0);
		BUG_ON(order!=0);
		order = ilog2(8);
		BUG_ON(order!=3);
		order = ilog2(16);
		BUG_ON(order!=4);
	}

	return EXIT_SUCCESS;
}
