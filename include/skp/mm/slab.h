//
//  slab.h
//
//  Created by 周凯 on 2019/3/4.
//  Copyright © 2019 zhoukai. All rights reserved.
//

#ifndef __US_SLAB_H__
#define __US_SLAB_H__

/*
 * SLUB : A Slab allocator without object queues.
 *
 * (C) 2007 SGI, Christoph Lameter <clameter@sgi.com>
 */

#include <stdarg.h>
#include "../utils/config.h"
#include "../utils/compiler.h"

__BEGIN_DECLS

struct umem_cache_s;
typedef struct umem_cache_s umem_cache_t;

enum {
	/*行为标志*/
	SLAB_HWCACHE_ALIGN = 0x0001U, /* 缓存线对齐*/
	SLAB_UNMERGEABLE = 0x0002U, /* 不能和其他创建分配器的请求合并*/
	SLAB_PANIC = 0x0004U, /* 一旦分配失败就杀死进程*/
	SLAB_NONDISCARD = 0x0008U, /* 不主动丢弃分配的页*/
	SLAB_RECLAIM = 0x0010U, /*在分配页时，主动压缩所有的 slab 分配器*/
	__SLAB_SHRINKING = 0x0020U, /*内部使用*/

	/*初始化状态*/
	SLAB_DOWN = 0, /* 初始化状态*/
	SLAB_PARTIAL, /* 可以创建缓存分配器*/
	SLAB_UP, /* 可以使用全局分配器了*/
	SLAB_COMP, /* 完全初始化，替换了静态分配器命名，并且内存泄漏检测模块可用
				* (如果已启用)*/
};

extern int slab_state;
extern void __umem_cache_init(void);
static inline void umem_cache_init(void)
{
	if (skp_likely(READ_ONCE(slab_state) >= SLAB_UP))
		return;
	__umem_cache_init();
}

/*
 * 以下回收函数不能在 pthread_key_create() 的析构中回调
 * 因为TLS段已经不存在，会导致段错误
 */

/*回收线程资源，一般在线程退出 或 将要长时间休眠时调用 */
extern void umem_cache_reclaim(void);
/*收缩缓存，也会调用 umem_cache_reclaim() */
extern void umem_cache_shrink(umem_cache_t *);
extern void umem_cache_shrink_all(void);

////////////////////////////////////////////////////////////////////////////////
extern umem_cache_t *umem_cache_create(const char *name, size_t s, size_t align,
		uint16_t flags);
extern bool umem_cache_destroy(umem_cache_t *);

extern size_t usize(const void *object);
extern void *__umem_cache_alloc(struct umem_cache_s *s, const char*, int);
/*不会使用缓存，直接释放到slab对象链表上*/
extern void umem_cache_free(struct umem_cache_s *s, const void *x);

////////////////////////////////////////////////////////////////////////////////

extern void *__umalloc(size_t size, const char*, int);
extern void *__urealloc(const void *p, size_t size, const char*, int);
extern void *__ucalloc(size_t n, size_t size, const char*, int);
/*会使用TLS缓存加速*/
extern void ufree(const void*);

/*忽略TLS缓存，直接释放到 slab 页*/
extern void __ufree(const void *);
////////////////////////////////////////////////////////////////////////////////
/*基于 umalloc 的字符串辅助函数*/
/**格式化拷贝*/
extern char *__uvasprintf(const char *, int, const char *fmt, va_list ap)
__printf(3, 0);
extern char *__ustrdup(const char *, int, const char *ptr);
extern char *__uasprintf(const char*, int, const char *fmt, ...) __printf(3, 4);

////////////////////////////////////////////////////////////////////////////////
/*分配包装宏，用于开启内存泄漏检测*/
#define umem_cache_alloc(s) __umem_cache_alloc((s), __FILE__, __LINE__)
#define umalloc(l) __umalloc((l), __FILE__, __LINE__)
#define urealloc(p, l) __urealloc((p), (l), __FILE__, __LINE__)
#define ucalloc(n, l) __ucalloc((n), (l), __FILE__, __LINE__)
#define uvasprintf(f, ap) __uvasprintf(__FILE__, __LINE__, (f), (ap))
#define ustrdup(p) __ustrdup(__FILE__, __LINE__, (p))
#define uasprintf(f, ...) __uasprintf(__FILE__, __LINE__, (f), ##__VA_ARGS__)
////////////////////////////////////////////////////////////////////////////////

#ifdef UMALLOC_MANGLE
# define malloc(s) umalloc((s))
# define free(p) ufree((p))
# define realloc(p, s) urealloc((p), (s))
# define calloc(n, s) ucalloc((n), (s))
# undef strdup
# define strdup(p) ustrdup(p)
# define aligned_alloc(a, s)									\
({																\
	void *_p; typeof(a) _a = (a); typeof(s) _s = (s);			\
	_s = (_s + (_a - 1)) & (~(_a - 1)); _p = umalloc(_s); _p;	\
})
# define tls_free(x) __ufree((x))
#else
# define tls_free(x) free((x))
#endif


__END_DECLS

////////////////////////////////////////////////////////////////////////////////
#endif /* __US_SLAB_H__ */
