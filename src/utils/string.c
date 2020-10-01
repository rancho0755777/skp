//
//  string.c
//  skp
//
//  Created by 周凯 on 2019/11/25.
//

#include <skp/utils/string.h>
#include <skp/mm/slab.h>

int sstr_build(struct sstr *sstr, const char *str, ssize_t len)
{
	char *ptr;

	if (len < 0)
		len = strlen(str);

	if (len + 1 > sizeof(sstr->_data)) {
		sstr->data = malloc(len + 1);
		if (skp_unlikely(!sstr->data))
			return -ENOMEM;
	}

	sstr->len = len;
	ptr = (char*)deref_sstr(sstr);
	if (skp_likely(len))
		memcpy(ptr, str, len);
	ptr[len] = '\0';

	return 0;
}

void sstr_finit(struct sstr *sstr)
{
	if (sstr->len + 1 > sizeof(sstr->_data))
		free(sstr->data);
}

int str2int(const char *ptr, ssize_t l, int64_t *value)
{
	bool isreg = false;
	size_t ll = 0;
	uint64_t v = 0;
	if (skp_unlikely(!l))
		return -EINVAL;

	if (l < 0)
		l = strlen(ptr);

	if (*ptr == '-') {
		isreg = true;
		ptr++;
		l--;
	}

	for (ssize_t i = 0; i < l; i++) {
		if (skp_unlikely(ptr[i] < '0' || ptr[i] > '9'))
			return -EINVAL;
		v *= 10;
		v += ptr[i] - '0';
		ll++;
	}

	log_debug("string to integer: %.*s -> %llu", l + 1, ptr, v);

	if (isreg) {
		if (skp_unlikely(v >= (uint64_t)S64_MAX))
			return -EINVAL;
		*value = - (int64_t)v;
	} else {
		*value = (int64_t)v;
	}

	return skp_likely(ll) ? (isreg ? 1 : 0) : -EINVAL;
}

int str2float(const char *ptr, ssize_t l, int64_t *value, int multi)
{
	size_t ll = 0;
	bool isreg = false, e_isreg = false;
	int multi_b = 0, e_multi = 1, iv_b = 0, dv_b = 0, ev_b = 0, *bptr;
	int64_t v, iv = 0,/*整数部分*/ dv = 0, /*小数部分*/ ev = 0, /*指数部分*/*pi = &iv;

	if (skp_unlikely(!l || multi < 1))
		return -EINVAL;

	if (l < 0)
		l = strlen(ptr);

	if (*ptr == '-') {
		isreg = true;
		ptr++;
		l--;
		if (!l)
			return -EINVAL;
	}
	bptr = &iv_b;
	for (ssize_t i = 0; i < l; i++) {
		if (ptr[i] < '0' || ptr[i] > '9') {
			if (ptr[i] == '.' && pi != &dv) {
				pi = &dv;
				bptr = &dv_b;
				continue;
			} else if ((ptr[i] == 'E' || ptr[i] == 'e') && pi != &ev) {
				if ((i+1) < l && (ptr[i+1] == '-' || ptr[i+1] == '+')) {
					if (ptr[++i] == '-')
						e_isreg = true;
				}
				pi = &ev;
				bptr = &ev_b;
				continue;
			}
			return -EINVAL;
		}
		(*pi) *= 10;
		(*pi) += ptr[i] - '0';
		(*bptr)++;
		ll++;
	}

	/*不允许 1.0E-0 这样的科学计数法存在*/
	if (e_isreg && !ev)
		return -EINVAL;

	if (ev) {
		/*科学计数法 非指数部分必须 是 1<=a<10*/
		if (iv < 1 || iv > 9)
			return -EINVAL;
		if (ev > 18)
			return -EINVAL;

		/*科学计数法处理*/
		for (int i = 0; i < ev; i++) {
			e_multi *= 10;
		}

		/*非负数则扩大*/
		if (!e_isreg) {
			multi *= e_multi;
		} else {
			multi /= e_multi;
			if (!multi)
				return -EINVAL;
		}
	}

	if (dv) {
		for (int i = multi - 1; i > 0; i /= 10) {
			multi_b++;
			if (WARN_ON(i%10!=9))
				return -EINVAL;
		}

		if (dv_b < multi_b) {
			while (dv_b++ < multi_b) dv *= 10;
		} else {
			while (dv_b-- > multi_b) dv /= 10;
		}
	} else {
		for (int i = multi - 1; i > 0; i /= 10) {
			if (WARN_ON(i%10!=9))
				return -EINVAL;
		}
	}

	iv = ((volatile uint64_t)iv) * multi;
	iv = ((volatile uint64_t)iv) / multi;
	iv = ((volatile uint64_t)iv) * multi;

	v = iv + dv;
	*value = isreg?-v:v;
	log_debug("string to float : %.*s -> %c%llu", l + 1, ptr, isreg?'-':' ', v);
	return skp_likely(ll) ? (isreg ? 1 : 0) : -EINVAL;
}

#ifndef __linux__
char *strchrnul(const char *s, int c)
{
	while (*s && *s != (char)c)
		s++;
	return (char *)s;
}
#endif
