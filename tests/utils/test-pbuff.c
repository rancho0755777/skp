#include <skp/utils/pbuff.h>

struct object {
	uref_t refs;
};

struct mybuff {
	uint8_t *ptr;
	struct pbuff buff;
	struct object *obj;
};

static inline int obj_get(struct object *obj)
{
	log_debug("get object");
	return uref_get_unless_zero(&obj->refs) ? 0 : -EINVAL;
}

static void obj_free(struct uref *__refs)
{
	log_debug("free object");
}

static inline void obj_put(struct object *obj)
{
	log_debug("put object");
	uref_put(&obj->refs, obj_free);
}

#define tomybuff(pb) \
	container_of((pb), struct mybuff, buff)

static struct pbuff *mybuff_constructor(void *user)
{
	struct mybuff *mpb = malloc(sizeof(*mpb));
	mpb->ptr = NULL;
	mpb->obj = user;
	mpb->buff.user = NULL;
	if (user)
		obj_get(mpb->obj);
	return &mpb->buff;
}

static void mybuff_destructor(struct pbuff *pb)
{
	struct mybuff *mpb = tomybuff(pb);
	obj_put(mpb->obj);
	free(mpb);
}

static void mybuff_clone(struct pbuff *npb, struct pbuff *opb)
{
	struct mybuff *nmpb = tomybuff(npb);
	struct mybuff *ompb = tomybuff(opb);
	nmpb->ptr = ompb->ptr;
	BUG_ON(obj_get(ompb->obj));
	nmpb->obj = ompb->obj;
}

static void mybuff_copy(struct pbuff *npb, struct pbuff *opb)
{
	struct mybuff *nmpb = tomybuff(npb);
	struct mybuff *ompb = tomybuff(opb);
	ssize_t offset = pb_head(&nmpb->buff) - pb_head(&ompb->buff);
	nmpb->ptr = ompb->ptr + offset;
	BUG_ON(obj_get(ompb->obj));
	nmpb->obj = ompb->obj;
}

static void mybuff_expand(struct pbuff *pb, ssize_t offset)
{
	struct mybuff *mpb = tomybuff(pb);
	mpb->ptr += offset;
}

const struct pb_ops mybuff_ops = {
	.constructor = mybuff_constructor,
	.destructor = mybuff_destructor,
	.clone = mybuff_clone,
	.copy = mybuff_copy,
	.expand = mybuff_expand,
};

int main(void)
{
	struct pbuff *pb;
	struct mybuff *mybuff, *clone, *copy;
	uint8_t *ptr, old;
	struct object obj;

	uref_init(&obj.refs);
	pb = __alloc_pb(10, &mybuff_ops, &obj);
	BUG_ON(!pb);
	BUG_ON(pb_tailroom(pb) < 10);
	mybuff = tomybuff(pb);
	obj_put(&obj);

	BUG_ON(uref_read(&mybuff->obj->refs) != 1);
	BUG_ON(pb_shared(&mybuff->buff));
	BUG_ON(pb_cloned(&mybuff->buff));
	BUG_ON(pb_headlen(&mybuff->buff));

	ptr = pb_putdata(&mybuff->buff, 6);
	BUG_ON(!ptr);
	snprintf((char*)ptr, 6, "%s", "Hello");
	pb_popdata(&mybuff->buff, 1);

	mybuff->ptr = ptr = pb_putdata(&mybuff->buff, 6);
	BUG_ON(!ptr);
	snprintf((char*)ptr, 6, "%s", "World");
	pb_popdata(&mybuff->buff, 1);

	BUG_ON(pb_headlen(&mybuff->buff) != 10);
	BUG_ON(memcmp(pb_data(&mybuff->buff), "HelloWorld", 10));

	BUG_ON(pb_expand_head(&mybuff->buff, 10, 10));
	BUG_ON(pb_headroom(&mybuff->buff) < 10);
	BUG_ON(pb_tailroom(&mybuff->buff) < 10);

	/*为了示范，进行了一次野指针操作，要非常小心，有可能导致程序崩溃
	 * 为了测试 pb_expand_head() 回调的正确性
	 */
	old = *ptr;
	static_mb();
	*ptr = '\0';
	BUG_ON(memcmp(mybuff->ptr, "World", 5));
	*ptr = old;

	pb = pb_share_check(&mybuff->buff);
	BUG_ON(pb != &mybuff->buff);

	pb_get(&mybuff->buff);
	pb = pb_share_check(&mybuff->buff);
	BUG_ON(!pb);
	BUG_ON(pb == &mybuff->buff);
	BUG_ON(uref_read(&obj.refs) != 2);
	BUG_ON(memcmp(mybuff->ptr, "World", 5));

	clone = tomybuff(pb);
	BUG_ON(clone->obj != mybuff->obj);
	BUG_ON(memcmp(clone->ptr, "World", 5));

	pb = pb_copy(&clone->buff);
	BUG_ON(!pb);
	copy = tomybuff(pb);
	BUG_ON(clone->obj != copy->obj);
	BUG_ON(memcmp(clone->ptr, "World", 5));
	BUG_ON(memcmp(copy->ptr, "World", 5));

	free_pb(&mybuff->buff);
	free_pb(&copy->buff);
	free_pb(&clone->buff);

	BUG_ON(uref_read(&obj.refs));

	/*编解码*/
	pb = __alloc_pb(128, NULL, NULL);

	uint8_t var8;
	uint16_t var16;
	uint32_t var32;
	uint64_t var64;

	int rc;
	rc = pb_write_uint8(pb, 8);
	BUG_ON(rc);
	rc = pb_write_uint16(pb, 16);
	BUG_ON(rc);
	rc = pb_write_uint32(pb, 32);
	BUG_ON(rc);
	rc = pb_write_uint64(pb, 64);
	BUG_ON(rc);

	rc = pb_read_uint8(pb, &var8);
	BUG_ON(rc);
	rc = pb_read_uint16(pb, &var16);
	BUG_ON(rc);
	rc = pb_read_uint32(pb, &var32);
	BUG_ON(rc);
	rc = pb_read_uint64(pb, &var64);
	BUG_ON(rc);

	BUG_ON(var8 != 8);
	BUG_ON(var16 != 16);
	BUG_ON(var32 != 32);
	BUG_ON(var64 != 64);

	free_pb(pb);
	
	return 0;
}
