//
//  string.h
//  skp
//
//  Created by 周凯 on 2019/11/25.
//

#ifndef __US_STRING_H__
#define __US_STRING_H__

#include "uref.h"
#include "utils.h"

__BEGIN_DECLS

/*简单字符串，对于小于8个字节的字符串，直接使用long long 存储*/
struct sstr {
	union {
		char *data;
		char _data[sizeof(uint64_t)];
	};
	uint32_t len;
};

static inline void sstr_init(struct sstr *sstr)
{
	sstr->len = 0;
	sstr->data = NULL;
}

extern void sstr_finit(struct sstr *sstr);
extern int sstr_build(struct sstr *sstr, const char *str, ssize_t len);

static inline uint32_t sstr_length(const struct sstr *sstr)
{
	return skp_likely(sstr) ? sstr->len : 0;
}

static inline const char* deref_sstr(const struct sstr *sstr)
{
	if (sstr->len + 1 > sizeof(sstr->_data))
		return (char*)sstr->data;
	return (char*)&sstr->_data[0];
}

static inline int sstr_copy(struct sstr *dst, const struct sstr *src)
{
	return sstr_build(dst, deref_sstr(src), src->len);
}

static inline const char * deref_sstr_print(const struct sstr *sstr)
{
	return sstr->len?deref_sstr(sstr):"null";
}

static inline bool sstr_equal(const struct sstr *ss1, const struct sstr *ss2)
{
	const char *s1, *s2;
	if (ss1->len != ss2->len)
		return false;
	s1 = deref_sstr(ss1);
	s2 = deref_sstr(ss2);
	return !memcmp(s1, s2, ss1->len) ? true : false;
}

////////////////////////////////////////////////////////////////////////////////
/*精度为4的浮点数，乘以10000后用整数表示*/

/**负数返回1，正数返回0，否则返回负数*/
extern int str2int(const char *ptr, ssize_t l, int64_t *value);
/**负数返回1，正数返回0，否则返回负数 */
extern int str2float(const char *ptr, ssize_t l, int64_t *value, int multi);

#ifndef __linux__
////////////////////////////////////////////////////////////////////////////////
/**
 * strchrnul - Find and return a character in a string, or end of string
 * @s: The string to be searched
 * @c: The character to search for
 *
 * Returns pointer to first occurrence of 'c' in s. If c is not found, then
 * return a pointer to the null byte at the end of s.
 */
extern char *strchrnul(const char *s, int c);
#endif

__END_DECLS

#endif /* string_h */
