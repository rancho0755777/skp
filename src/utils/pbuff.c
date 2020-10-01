#include <skp/utils/pbuff.h>
#include <skp/mm/slab.h>

static struct pbuff *__def_constructor(void *_)
{
	return malloc(sizeof(struct pbuff));
}

static void __def_destructor(struct pbuff *pb)
{
	free(pb);
}

static const struct pb_ops pb_defops = {
	.constructor = __def_constructor,
	.destructor = __def_destructor,
	.clone = NULL,
	.copy = NULL,
	.expand = NULL,
};

struct pbuff *__alloc_pb(size_t size, const struct pb_ops *pb_ops,
		void *user)
{
	struct pbuff *pb;
	uint8_t *data;

	pb_ops = pb_ops?:&pb_defops;
	if (WARN_ON(!pb_ops->constructor))
		return NULL;

	/*在构造函数内初始化继承类的字段*/
	pb = pb_ops->constructor(user);
	if (skp_unlikely(!pb))
		return NULL;

	/*user字段 完全由构造函数管理*/
	pb->pb_ops = pb_ops;

	size = ALIGN(size, sizeof(struct pb_shared_info));
	data = malloc(size + sizeof(struct pb_shared_info));
	if (skp_unlikely(!data)) {
		if (pb_ops->destructor)
			pb_ops->destructor(pb);
		return NULL;
	}

	/*初始化基类*/
	uref_init(&pb->users);
	pb->data = data;
	pb->tail = data;
	pb->end = data + size;
	pb->flags = 0;

	uref_init(&(pb_shinfo(pb)->dataref));
	pb_shinfo(pb)->datalen = (uint32_t)size;

	log_debug("alloc pb : %p/%p(%zu)", pb, data, size);

	return pb;
}

static inline void pb_release_data(struct pbuff *pb)
{
	if (__uref_put(&(pb_shinfo(pb)->dataref))) {
		log_debug("free shared data : %p/%p(%u)", pb, pb_head(pb),
			pb_shinfo(pb)->datalen);
		free(pb_head(pb));
	}
}

void __free_pb(uref_t *refs)
{
	struct pbuff *pb = container_of(refs, struct pbuff, users);
	BUG_ON(uref_read(refs)!=0);

	pb_release_data(pb);
	if (pb->pb_ops->destructor)
		pb->pb_ops->destructor(pb);
}

struct pbuff *pb_clone(struct pbuff *pb)
{
	struct pbuff *new;

	/*alloc new control struct to wrapping up the shared data*/
	new = pb->pb_ops->constructor(pb->user);
	if (skp_unlikely(!new))
		return NULL;

	/*基类的字段相同*/
	memcpy(new, pb, sizeof(*pb));

	uref_get(&(pb_shinfo(pb)->dataref));
	pb->cloned = 1;

	uref_init(&new->users);
	new->cloned = 1;
	/*
	 * other special assignment
	 * 使用回调来初始化继承类
	 */
	if (pb->pb_ops->clone)
		pb->pb_ops->clone(new, pb);

	return new;
}

static inline int pb_copy_bits(const struct pbuff *pb,
	ssize_t offset, void *to, uint32_t len)
{
	ssize_t copy, start = pb_headlen(pb);

	if (skp_unlikely(offset > (ssize_t)start - len))
		return -EFAULT;

	if ((copy = start - offset) > 0) {
		if (skp_unlikely(copy > len))
			copy = len;
		memcpy(to, pb->data + offset, copy);
		if (skp_likely((len -= (uint32_t)copy) == 0))
			return 0;
		to += copy;
		offset += copy;
	}

	if (skp_unlikely(len))
		return -EFAULT;

	return 0;
}

struct pbuff *pb_copy(struct pbuff *pb)
{
	struct pbuff *new;
	ssize_t headerlen;

	headerlen = pb->data - pb_head(pb);
#ifdef DEBUG
	BUG_ON(ALIGN(pb->end - pb_head(pb), sizeof(struct pb_shared_info)) !=
		pb_shinfo(pb)->datalen);
#endif
	new = __alloc_pb(pb_size(pb), pb->pb_ops, pb->user);
	if (skp_unlikely(!new))
		return NULL;

	/*meta信息*/
	/*data信息*/
	pb_reserve(new, (uint32_t)headerlen);
	pb_putdata(new, pb_headlen(pb));

	BUG_ON(pb_copy_bits(pb, -headerlen,
		pb_head(new), (uint32_t)headerlen + pb_headlen(pb)));

	/*使用回调去初始化继承类*/
	if (new->pb_ops->copy)
		new->pb_ops->copy(new, pb);

	log_debug("pb copy : %p -> %p", pb, new);
	return new;
}

struct pbuff *pb_share(struct pbuff *src, ssize_t l, void *user,
		const struct pb_ops* ops)
{
	struct pbuff *pb;

	if (skp_unlikely(!l))
		return NULL;

	pb = ops->constructor(user);
	if (skp_unlikely(!pb))
		return NULL;

	src->cloned = 1;
	pb->cloned = 1;
	pb->pb_ops = ops;
	pb->end = src->end;

	uref_init(&pb->users);
	uref_get(&pb_shinfo(src)->dataref);

	if (l > 0) {
		pb->data = src->data;
		pb->tail = src->data + l;
		BUG_ON(pb->tail > pb->end);
	} else {
		pb->data = src->data + l;
		pb->tail = src->data;
		BUG_ON(pb->data < pb_head(pb));
	}

	return pb;
}

int __pb_expand_head(struct pbuff *pb, size_t nhead, size_t ntail)
{
	uint8_t *data;
	ssize_t offset;
	size_t size;

	BUG_ON(nhead > U32_MAX);
	BUG_ON(ntail > U32_MAX);

	if (WARN_ON(pb_shared(pb)))
		return -EPERM;

	size = nhead + pb_size(pb) + ntail;
	size = ALIGN(size, sizeof(struct pb_shared_info));

	data = malloc(size + sizeof(struct pb_shared_info));
	if (skp_unlikely(!data))
		return -ENOMEM;

	/*copy user data*/
	memcpy(data + nhead, pb_head(pb), pb_size(pb));
	/*注意基准点，是 data + nhead */
	offset = data + nhead - pb_head(pb);
	/*pb_head 动态计算，必须在计算偏移后，释放原来的数据区*/
	pb_release_data(pb);

	pb->end = data + size;
	pb->data += offset;
	pb->tail += offset;

	pb->cloned = 0;
	uref_init(&pb_shinfo(pb)->dataref);
	pb_shinfo(pb)->datalen = (uint32_t)size;

	/*使用回调来初始化继承类*/
	if (pb->pb_ops->expand)
		pb->pb_ops->expand(pb, offset);

	log_debug("alloc shared data : %p/%p(%u)",
		pb, pb_head(pb), pb_shinfo(pb)->datalen);
	return 0;
}
