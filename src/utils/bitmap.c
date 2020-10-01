#define _GNU_SOURCE
#include <string.h>
#include <skp/utils/utils.h>
#include <skp/utils/string.h>
#include <skp/utils/bitmap.h>

/*
 * This is a common helper function for find_next_bit, find_next_zero_bit, and
 * find_next_and_bit. The differences are:
 *  - The "invert" argument, which is XORed with each fetched word before
 *    searching it for one bits.
 *  - The optional "addr2", which is anded with "addr1" if present.
 */
unsigned long __find_next_bit(const unsigned long *addr1,
	const unsigned long *addr2, unsigned long nbits,
	unsigned long start, unsigned long invert)
{
	unsigned long tmp;

	if (skp_unlikely(start >= nbits))
		return nbits;

	tmp = addr1[start / BITS_PER_LONG];
	if (addr2)
		tmp &= addr2[start / BITS_PER_LONG];
	tmp ^= invert;

	/* Handle 1st word. */
	tmp &= BITMAP_FIRST_WORD_MASK(start);
	start = round_down(start, BITS_PER_LONG);

	while (!tmp) {
		start += BITS_PER_LONG;
		if (start >= nbits)
			return nbits;

		tmp = addr1[start / BITS_PER_LONG];
		if (addr2)
			tmp &= addr2[start / BITS_PER_LONG];
		tmp ^= invert;
	}

	return min(start + __ffsl(tmp), nbits);
}

/*
 * Find the first set bit in a memory region.
 */
unsigned long _find_first_bit(const unsigned long *addr, unsigned long size)
{
	unsigned long idx;

	for (idx = 0; idx * BITS_PER_LONG < size; idx++) {
		if (addr[idx])
			return min(idx * BITS_PER_LONG + __ffsl(addr[idx]), size);
	}

	return size;
}

/*
 * Find the first cleared bit in a memory region.
 */
unsigned long _find_first_zero_bit(const unsigned long *addr, unsigned long size)
{
	unsigned long idx;

	for (idx = 0; idx * BITS_PER_LONG < size; idx++) {
		if (addr[idx] != ~0UL)
			return min(idx * BITS_PER_LONG + ffzl(addr[idx]), size);
	}

	return size;
}

unsigned long find_last_bit(const unsigned long *addr, unsigned long size)
{
	if (size) {
		unsigned long val = BITMAP_LAST_WORD_MASK(size);
		unsigned long idx = (size-1) / BITS_PER_LONG;

		do {
			val &= addr[idx];
			if (val)
				return idx * BITS_PER_LONG + __flsl(val);

			val = ~0ul;
		} while (idx--);
	}
	return size;
}

#if BITS_PER_LONG == 32
unsigned int __bitmap_weight(const unsigned long *bitmap, unsigned long bits)
{
	unsigned int k, w = 0, lim = (unsigned int)(bits/BITS_PER_LONG);

	for (k = 0; k < lim; k++)
		w += hweight32(bitmap[k]);

	if (bits % BITS_PER_LONG)
		w += hweight32(bitmap[k] & BITMAP_LAST_WORD_MASK(bits));

	return w;
}
#else
unsigned int __bitmap_weight(const unsigned long *bitmap, unsigned long bits)
{
	unsigned int k, w = 0, lim = (unsigned int)(bits/BITS_PER_LONG);

	for (k = 0; k < lim; k++)
		w += hweight64(bitmap[k]);

	if (bits % BITS_PER_LONG)
		w += hweight64(bitmap[k] & BITMAP_LAST_WORD_MASK(bits));

	return w;
}
#endif

int __bitmap_equal(const unsigned long *bitmap1,
		const unsigned long *bitmap2, unsigned int bits)
{
	unsigned int k, lim = bits/BITS_PER_LONG;
	for (k = 0; k < lim; ++k)
		if (bitmap1[k] != bitmap2[k])
			return 0;

	if (bits % BITS_PER_LONG)
		if ((bitmap1[k] ^ bitmap2[k]) & BITMAP_LAST_WORD_MASK(bits))
			return 0;

	return 1;
}

void __bitmap_complement(unsigned long *dst,
		const unsigned long *src, unsigned int bits)
{
	unsigned int k, lim = bits/BITS_PER_LONG;
	for (k = 0; k < lim; ++k)
		dst[k] = ~src[k];

	if (bits % BITS_PER_LONG)
		dst[k] = ~src[k];
}

int __bitmap_and(unsigned long *dst, const unsigned long *bitmap1,
		const unsigned long *bitmap2, unsigned int bits)
{
	unsigned int k;
	unsigned int lim = bits/BITS_PER_LONG;
	unsigned long result = 0;

	for (k = 0; k < lim; k++)
		result |= (dst[k] = bitmap1[k] & bitmap2[k]);
	if (bits % BITS_PER_LONG)
		result |= (dst[k] = bitmap1[k] & bitmap2[k] &
				   BITMAP_LAST_WORD_MASK(bits));
	return result != 0;
}

void __bitmap_or(unsigned long *dst, const unsigned long *bitmap1,
		const unsigned long *bitmap2, unsigned int bits)
{
	unsigned int k;
	unsigned int nr = BITS_TO_LONGS(bits);

	for (k = 0; k < nr; k++)
		dst[k] = bitmap1[k] | bitmap2[k];
}

void __bitmap_xor(unsigned long *dst, const unsigned long *bitmap1,
		const unsigned long *bitmap2, unsigned int bits)
{
	unsigned int k;
	unsigned int nr = BITS_TO_LONGS(bits);

	for (k = 0; k < nr; k++)
		dst[k] = bitmap1[k] ^ bitmap2[k];
}

int __bitmap_andnot(unsigned long *dst, const unsigned long *bitmap1,
		const unsigned long *bitmap2, unsigned int bits)
{
	unsigned int k;
	unsigned int lim = bits/BITS_PER_LONG;
	unsigned long result = 0;

	for (k = 0; k < lim; k++)
		result |= (dst[k] = bitmap1[k] & ~bitmap2[k]);
	if (bits % BITS_PER_LONG)
		result |= (dst[k] = bitmap1[k] & ~bitmap2[k] &
				   BITMAP_LAST_WORD_MASK(bits));
	return result != 0;
}

int __bitmap_intersects(const unsigned long *bitmap1,
		const unsigned long *bitmap2, unsigned int bits)
{
	unsigned int k, lim = bits/BITS_PER_LONG;
	for (k = 0; k < lim; ++k)
		if (bitmap1[k] & bitmap2[k])
			return 1;

	if (bits % BITS_PER_LONG)
		if ((bitmap1[k] & bitmap2[k]) & BITMAP_LAST_WORD_MASK(bits))
			return 1;
	return 0;
}

int __bitmap_subset(const unsigned long *bitmap1,
		const unsigned long *bitmap2, unsigned int bits)
{
	unsigned int k, lim = bits/BITS_PER_LONG;
	for (k = 0; k < lim; ++k)
		if (bitmap1[k] & ~bitmap2[k])
			return 0;

	if (bits % BITS_PER_LONG)
		if ((bitmap1[k] & ~bitmap2[k]) & BITMAP_LAST_WORD_MASK(bits))
			return 0;
	return 1;
}

void __bitmap_set(unsigned long *map, unsigned int start, int len)
{
	unsigned long *p = map + BIT_WORD(start);
	const unsigned int size = start + len;
	int bits_to_set = BITS_PER_LONG - (start % BITS_PER_LONG);
	unsigned long mask_to_set = BITMAP_FIRST_WORD_MASK(start);

	while (len - bits_to_set >= 0) {
		*p |= mask_to_set;
		len -= bits_to_set;
		bits_to_set = BITS_PER_LONG;
		mask_to_set = ~0UL;
		p++;
	}
	if (len) {
		mask_to_set &= BITMAP_LAST_WORD_MASK(size);
		*p |= mask_to_set;
	}
}

void __bitmap_clear(unsigned long *map, unsigned int start, int len)
{
	unsigned long *p = map + BIT_WORD(start);
	const unsigned int size = start + len;
	int bits_to_clear = BITS_PER_LONG - (start % BITS_PER_LONG);
	unsigned long mask_to_clear = BITMAP_FIRST_WORD_MASK(start);

	while (len - bits_to_clear >= 0) {
		*p &= ~mask_to_clear;
		len -= bits_to_clear;
		bits_to_clear = BITS_PER_LONG;
		mask_to_clear = ~0UL;
		p++;
	}
	if (len) {
		mask_to_clear &= BITMAP_LAST_WORD_MASK(size);
		*p &= ~mask_to_clear;
	}
}

/**
 * __bitmap_shift_right - logical right shift of the bits in a bitmap
 *   @dst : destination bitmap
 *   @src : source bitmap
 *   @shift : shift by this many bits
 *   @nbits : bitmap size, in bits
 *
 * Shifting right (dividing) means moving bits in the MS -> LS bit
 * direction.  Zeros are fed into the vacated MS positions and the
 * LS bits shifted off the bottom are lost.
 */
void __bitmap_shift_right(unsigned long *dst, const unsigned long *src,
		unsigned shift, unsigned nbits)
{
	unsigned k, lim = BITS_TO_LONGS(nbits);
	unsigned off = shift/BITS_PER_LONG, rem = shift % BITS_PER_LONG;
	unsigned long mask = BITMAP_LAST_WORD_MASK(nbits);
	for (k = 0; off + k < lim; ++k) {
		unsigned long upper, lower;

		/*
		 * If shift is not word aligned, take lower rem bits of
		 * word above and make them the top rem bits of result.
		 */
		if (!rem || off + k + 1 >= lim)
			upper = 0;
		else {
			upper = src[off + k + 1];
			if (off + k + 1 == lim - 1)
				upper &= mask;
			upper <<= (BITS_PER_LONG - rem);
		}
		lower = src[off + k];
		if (off + k == lim - 1)
			lower &= mask;
		lower >>= rem;
		dst[k] = lower | upper;
	}
	if (off)
		memset(&dst[lim - off], 0, off*sizeof(unsigned long));
}
/**
 * __bitmap_shift_left - logical left shift of the bits in a bitmap
 *   @dst : destination bitmap
 *   @src : source bitmap
 *   @shift : shift by this many bits
 *   @nbits : bitmap size, in bits
 *
 * Shifting left (multiplying) means moving bits in the LS -> MS
 * direction.  Zeros are fed into the vacated LS bit positions
 * and those MS bits shifted off the top are lost.
 */

void __bitmap_shift_left(unsigned long *dst, const unsigned long *src,
		unsigned int shift, unsigned int nbits)
{
	int k;
	unsigned int lim = BITS_TO_LONGS(nbits);
	unsigned int off = shift/BITS_PER_LONG, rem = shift % BITS_PER_LONG;
	for (k = lim - off - 1; k >= 0; --k) {
		unsigned long upper, lower;

		/*
		 * If shift is not word aligned, take upper rem bits of
		 * word below and make them the bottom rem bits of result.
		 */
		if (rem && k > 0)
			lower = src[k - 1] >> (BITS_PER_LONG - rem);
		else
			lower = 0;
		upper = src[k] << rem;
		dst[k + off] = lower | upper;
	}
	if (off)
		memset(dst, 0, off*sizeof(unsigned long));
}

#if BITS_PER_LONG == 64
/**
 * bitmap_from_arr32 - copy the contents of u32 array of bits to bitmap
 *	@bitmap: array of unsigned longs, the destination bitmap
 *	@buf: array of u32 (in host byte order), the source bitmap
 *	@nbits: number of bits in @bitmap
 */
void bitmap_from_arr32(unsigned long *bitmap, const uint32_t *buf,
		unsigned int nbits)
{
	unsigned int i, halfwords;

	if (!nbits)
		return;

	halfwords = DIV_ROUND_UP(nbits, 32);
	for (i = 0; i < halfwords; i++) {
		bitmap[i/2] = (unsigned long) buf[i];
		if (++i < halfwords)
			bitmap[i/2] |= ((unsigned long) buf[i]) << 32;
	}

	/* Clear tail bits in last word beyond nbits. */
	if (nbits % BITS_PER_LONG)
		bitmap[(halfwords - 1) / 2] &= BITMAP_LAST_WORD_MASK(nbits);
}

/**
 * bitmap_to_arr32 - copy the contents of bitmap to a u32 array of bits
 *	@buf: array of u32 (in host byte order), the dest bitmap
 *	@bitmap: array of unsigned longs, the source bitmap
 *	@nbits: number of bits in @bitmap
 */
void bitmap_to_arr32(uint32_t *buf, const unsigned long *bitmap,
		unsigned int nbits)
{
	unsigned int i, halfwords;

	if (!nbits)
		return;

	halfwords = DIV_ROUND_UP(nbits, 32);
	for (i = 0; i < halfwords; i++) {
		buf[i] = (uint32_t) (bitmap[i/2] & UINT_MAX);
		if (++i < halfwords)
			buf[i] = (uint32_t) (bitmap[i/2] >> 32);
	}

	/* Clear tail bits in last element of array beyond nbits. */
	if (nbits % BITS_PER_LONG)
		buf[halfwords - 1] &= (uint32_t) (UINT_MAX >> ((-nbits) & 31));
}

#endif

int __bitmap_parselist(const char *buf, size_t buflen, unsigned long *bitmap,
		unsigned int nbits)
{
	unsigned int a, b, old_a, old_b;
	unsigned int group_size, used_size, off;
	int c, old_c, totaldigits, ndigits;
	int at_start, in_range, in_partial_range;

	totaldigits = c = 0;
	old_a = old_b = 0;
	group_size = used_size = 0;
	bitmap_zero(bitmap, nbits);

	do {
		at_start = 1;
		in_range = 0;
		in_partial_range = 0;
		a = b = 0;
		ndigits = totaldigits;

		/* Get the next cpu# or a range of cpu#'s */
		while (buflen) {
			old_c = c;
			c = *buf++;
			buflen--;
			if (isspace(c))
				continue;

			/* A '\0' or a ',' signal the end of a cpu# or range */
			if (c == '\0' || c == ',')
				break;
			/*
			* whitespaces between digits are not allowed,
			* but it's ok if whitespaces are on head or tail.
			* when old_c is whilespace,
			* if totaldigits == ndigits, whitespace is on head.
			* if whitespace is on tail, it should not run here.
			* as c was ',' or '\0',
			* the last code line has broken the current loop.
			*/
			if ((totaldigits != ndigits) && isspace(old_c))
				return -EINVAL;

			if (c == '/') {
				used_size = a;
				at_start = 1;
				in_range = 0;
				a = b = 0;
				continue;
			}

			if (c == ':') {
				old_a = a;
				old_b = b;
				at_start = 1;
				in_range = 0;
				in_partial_range = 1;
				a = b = 0;
				continue;
			}

			if (c == '-') {
				if (at_start || in_range)
					return -EINVAL;
				b = 0;
				in_range = 1;
				at_start = 1;
				continue;
			}

			if (!isdigit(c))
				return -EINVAL;

			b = b * 10 + (c - '0');
			if (!in_range)
				a = b;
			at_start = 0;
			totaldigits++;
		}
		if (ndigits == totaldigits)
			continue;
		if (in_partial_range) {
			group_size = a;
			a = old_a;
			b = old_b;
			old_a = old_b = 0;
		} else {
			used_size = group_size = b - a + 1;
		}
		/* if no digit is after '-', it's wrong*/
		if (at_start && in_range)
			return -EINVAL;
		if (!(a <= b) || group_size == 0 || !(used_size <= group_size))
			return -EINVAL;
		if (b >= nbits)
			return -ERANGE;
		while (a <= b) {
			off = min(b - a + 1, used_size);
			bitmap_set(bitmap, a, off);
			a += group_size;
		}
	} while (buflen && c == ',');
	return 0;
}

int bitmap_parselist(const char *bp, unsigned long *maskp, unsigned int nmaskbits)
{
	char *nl  = strchrnul(bp, '\n');
	size_t len = nl - bp;
	return __bitmap_parselist(bp, len, maskp, nmaskbits);
}
