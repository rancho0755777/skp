#ifndef __US_PBUFF_H__
#define __US_PBUFF_H__

#include "utils.h"
#include "uref.h"

__BEGIN_DECLS

struct pbuff;
struct pb_ops {
	struct pbuff *(*constructor)(void *user);
	void (*destructor)(struct pbuff*);
	/*克隆成功后回调，用于设置继承类的字段*/
	void (*clone)(struct pbuff*n, struct pbuff*o);
	/*深拷贝成功后回调，用于设置继承类的字段*/
	void (*copy)(struct pbuff*n, struct pbuff*o);
	/*扩展成功后回调，用于设置继承类的字段*/
	void (*expand)(struct pbuff*, ssize_t offset);
};

struct pb_shared_info {
	uref_t dataref;
	uint32_t datalen;
	/*TODO : block data arrays*/
};

/*help to structure protocol data*/
struct pbuff {
	uref_t users;
	/*private data, must access by API*/
	union {
		uint32_t flags;
		struct {
			uint32_t cloned:1;
		};
	};
	uint8_t *data; /*data pointer with length*/
	uint8_t *tail;
	uint8_t *end;
	void *user;
	const struct pb_ops *pb_ops;
};

#define pb_data(pb) ((pb)->data)
#define pb_tail(pb) ((pb)->tail)
/*共享缓存信息*/
#define pb_shinfo(pb) ((struct pb_shared_info*)((pb)->end))
/*总空间大小*/
#define pb_size(pb) (pb_shinfo(pb)->datalen)
/*起始位置*/
#define pb_head(pb) ((pb)->end - pb_shinfo(pb)->datalen)
/*有效数据长度*/
#define pb_headlen(pb) ((size_t)(pb_tail(pb) - pb_data(pb)))
/*头部剩余空间*/
#define pb_headroom(pb) ((size_t)((pb)->data - pb_head(pb)))
/*尾部剩余空间*/
#define pb_tailroom(pb) ((size_t)((pb)->end - (pb)->tail))

static inline bool pb_shared(const struct pbuff *pb)
{
	return uref_read(&pb->users) != 1;
}

static inline bool pb_cloned(const struct pbuff *pb)
{
	return pb->cloned && uref_read(&pb_shinfo(pb)->dataref) != 1;
}

/*check if pbuff could be reused then reset user data, and return true*/
static inline bool pb_reset(struct pbuff *pb)
{
	if (WARN_ON(pb_shared(pb) || pb_cloned(pb)))
		return false;
	pb->flags = 0;
	pb->data = pb->tail = pb_head(pb);
	return true;
}

/*
 * 拷贝构造和克隆构造时，user一定为NULL
 * 然后通过拷贝和克隆回调去初始化新的pb
 * user 不会被存储
 */
extern struct pbuff *__alloc_pb(size_t size, const struct pb_ops *pb_ops,
		void *user);
extern void __free_pb(uref_t*);

static inline struct pbuff *alloc_pb(size_t size)
{
	return __alloc_pb(size, NULL, NULL);
}

static inline void free_pb(struct pbuff *pb)
{
	if (skp_likely(pb))
		uref_put(&pb->users, __free_pb);
}

static inline struct pbuff *pb_get(struct pbuff *pb)
{
	if (skp_likely(pb) && !uref_get_unless_zero(&pb->users))
		return NULL;
	return pb;
}

/*浅拷贝*/
extern struct pbuff *pb_clone(struct pbuff*);
/*深拷贝*/
extern struct pbuff *pb_copy(struct pbuff*);
/*
 * 共享从当前位置起的指定偏移长度的底层数据
 * 为负数则共享已消费的数据
 * 不能共享0长度的数据
 * 必须保证 长度是有效的 不能超出 共享缓存的头尾
 */
extern struct pbuff *pb_share(struct pbuff*,ssize_t,void*,const struct pb_ops*);

/*扩展核心，不检查剩余空间*/
extern int __pb_expand_head(struct pbuff *pb, size_t head, size_t tail);

/*真正发生扩展后会解除共享*/
static inline int pb_expand_head(struct pbuff *pb, size_t nhead, size_t ntail)
{
	if (pb_headroom(pb) >= nhead)
		nhead = 0;
	if (pb_tailroom(pb) >= ntail)
		ntail = 0;
	if (!nhead && !ntail)
		return 0;
	return __pb_expand_head(pb, nhead << 1, ntail << 1);
}

/*发现引用计数不为1，则克隆一份，并解除一次引用*/
static inline struct pbuff *pb_share_check(struct pbuff *pb)
{
	if (pb_shared(pb)) {
		struct pbuff *npb = pb_clone(pb);
		if (skp_unlikely(!npb))
			return NULL;
		free_pb(pb);
		pb = npb;
	}
	return pb;
}

/*发现克隆，则深拷贝一份*/
static inline struct pbuff *pb_unshare(struct pbuff *pb)
{
	struct pbuff *npb;
	if (!pb_cloned(pb))
		return pb;
	if (skp_unlikely(!(npb = pb_copy(pb))))
		return NULL;
	free_pb(pb);
	return npb;
}

/**尾部添加数据*/
static inline void *pb_putdata(struct pbuff *pb, size_t len)
{
	uint8_t *tmp = pb->tail;
	pb->tail += len;
	BUG_ON(pb->tail > pb->end);
	return tmp;
}
/**头部添加数据*/
static inline void *pb_pushdata(struct pbuff *pb, size_t len)
{
	pb->data -= len;
	BUG_ON(pb_head(pb) > pb->data);
	return pb->data;
}

/**消费尾部数据*/
static inline void *pb_popdata(struct pbuff *pb, size_t len)
{
	if (skp_unlikely(len > pb_headlen(pb)))
		return NULL;
	pb->tail -= len;
	return pb->tail;
}

/**消费头部数据*/
static inline void *pb_pulldata(struct pbuff *pb, size_t len)
{
	uint8_t *tmp = pb_data(pb);
	if (skp_unlikely(len > pb_headlen(pb)))
		return NULL;
	pb->data += len;
	return tmp;
}

/*头部保留空间，必须在创建初期使用，否则数据会出现混乱*/
static inline void pb_reserve(struct pbuff *pb, size_t len)
{
	BUG_ON(pb->data != pb_head(pb));
	pb->data += len;
	pb->tail += len;
	BUG_ON(pb->tail > pb->end);
}

/*从尾部截断数据为指定长度*/
static inline void pb_trimdata(struct pbuff *pb, size_t len)
{
	if (pb_headlen(pb) > len)
		pb->tail = pb->data + len;
}

////////////////////////////////////////////////////////////////////////////////
// 一般性读写辅助，读头、写尾。
// intel x86 平台运行进行 未对齐指针的解引用操作
// 其他平台需要转换
////////////////////////////////////////////////////////////////////////////////

#define pb_readptr(dstptr, srcptr)								\
do {															\
	uint8_t *__dp = (uint8_t *)(dstptr);						\
	uint8_t *__sp = (uint8_t *)(srcptr);						\
	BUILD_BUG_ON_NOT_POWER_OF_2(sizeof(*dstptr));				\
	if (((uintptr_t)(__sp))&(sizeof(*dstptr) - 1)) {			\
		switch (sizeof(*dstptr))								\
		{														\
		case 8:													\
			*__dp++ = *__sp++;									\
			*__dp++ = *__sp++;									\
			*__dp++ = *__sp++;									\
			*__dp++ = *__sp++;									\
		case 4:													\
			*__dp++ = *__sp++;									\
			*__dp++ = *__sp++;									\
		case 2:													\
			*__dp++ = *__sp++;									\
		case 1:													\
			*__dp = *__sp;										\
			break;												\
		default: BUG();											\
		}														\
	} else {													\
		*(typeof(dstptr))__dp = *(typeof(dstptr))__sp;			\
	}															\
} while(0)


#define pb_encode_decode(opt, var)								\
({																\
	typeof(var) __v = var;										\
	switch(sizeof(__v)) {										\
	case 1: break;												\
	case 2: __v = opt##s(__v);break;							\
	case 4: __v = opt##l(__v);break;							\
	case 8: __v = opt##ll(__v);break;							\
	default:BUG();												\
	}															\
	__v;														\
})

#define __def_pb_read_func(suffix, type)		 				\
static inline void __pb_read_##suffix(struct pbuff *pb,type *p)	\
{																\
	type *ptr = (type*)pb_pulldata((pb), sizeof(type));			\
	*p = pb_encode_decode(ntoh, *ptr);							\
}																\
static inline int pb_read_##suffix(struct pbuff *pb, type *p)	\
{																\
	if (skp_likely(pb_headlen(pb) >= sizeof(type))) {			\
		__pb_read_##suffix(pb, p);								\
		return 0;												\
	}															\
	return -ENODATA;											\
}

__def_pb_read_func(uint8, uint8_t)
__def_pb_read_func(uint16, uint16_t)
__def_pb_read_func(uint32, uint32_t)
__def_pb_read_func(uint64, uint64_t)

#undef __def_pb_read_func

static inline void __pb_read_bytes(struct pbuff *pb, void **pptr, size_t l)
{
	*pptr = pb_pulldata(pb, l);
}

static inline int pb_read_bytes(struct pbuff *pb, void **pptr, size_t l)
{
	if (skp_likely(pb_headlen(pb) >= l)) {
		__pb_read_bytes(pb, pptr, l);
		return 0;
	}
	return -ENODATA;
}

static inline int pb_prepare_write(struct pbuff *pb, size_t n)
{
	if (skp_unlikely(pb_headroom(pb) < n))
		return __pb_expand_head(pb, 0, max_t(uint32_t, n, pb_size(pb)/2));
	return 0;
}

#define __def_pb_write_func(suffix, type)		 				\
static inline void __pb_write_##suffix(struct pbuff *pb, type v)\
{																\
	type *ptr = (type*)pb_putdata((pb), sizeof(type));			\
	*ptr = pb_encode_decode(hton, v);							\
}																\
static inline int pb_write_##suffix(struct pbuff *pb, type v)	\
{																\
	if (skp_likely(!pb_prepare_write(pb, sizeof(type)))) {			\
		__pb_write_##suffix(pb, v);								\
		return 0;												\
	}															\
	return -ENOMEM;												\
}

__def_pb_write_func(uint8, uint8_t)
__def_pb_write_func(uint16, uint16_t)
__def_pb_write_func(uint32, uint32_t)
__def_pb_write_func(uint64, uint64_t)

#undef __def_pb_write_func

static inline void __pb_write_bytes(struct pbuff *pb, const void *ptr, size_t l)
{
	void *p = pb_putdata(pb, l);
	memcpy(p, ptr, l);
}

static inline int pb_write_bytes(struct pbuff *pb, const void *ptr, size_t l)
{
	if (skp_likely(!pb_prepare_write(pb, l))) {
		__pb_write_bytes(pb, ptr, l);
		return 0;
	}
	return -ENOMEM;
}

__END_DECLS

#endif
