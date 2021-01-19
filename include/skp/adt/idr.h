#ifndef __US_IDR_H__
#define __US_IDR_H__

#include "radix_tree.h"

__BEGIN_DECLS

/*整数 ID 分配器 参考 系统 pid 分配*/
struct idmap {
	atomic_t nr_free;
	uint32_t nr_bit;
	void *page;
};

struct idt {
	/*todo : 使用一个 last 分配ID的缓存池*/
	atomic_t nr_free;
	uint32_t offset;
	uint32_t nr_bit;
	uint32_t last;
	struct idmap *idmap;
};

static inline uint32_t idt_nr_free(const struct idt *idt)
{
	return atomic_read(&idt->nr_free);
}

static inline uint32_t idt_nr_ids(const struct idt *idt)
{
	return idt->nr_bit - atomic_read(&idt->nr_free);
}

/*
 * 可以指定id的起始ID号和结束ID号
 * @param start 闭区间
 * @param end 闭区间
 */
extern int idt_init(struct idt *idt, uint32_t start, uint32_t end);
extern void idt_destroy(struct idt *idt);

extern int idt_next(struct idt *idt, int _id);

extern int idt_alloc(struct idt *idt);
/*是否更新last*/
extern bool __idt_remove(struct idt *idt, int _id, bool ring);

static inline bool idt_remove(struct idt *idt, int _id)
{
	return __idt_remove(idt, _id, false);
}

static inline bool idt_ring_remove(struct idt *idt, int _id)
{
	return __idt_remove(idt, _id, true);
}

struct idr {
	struct idt idt;
	struct radix_tree_root rdtree;
};

static inline uint32_t idr_nr_free(const struct idr *idr)
{
	return atomic_read(&idr->idt.nr_free);
}

/*
 * TODO ：根据ID范围值设置多个大小的idr节点，以节约空间
 */
extern int idr_alloc(struct idr*, void *ptr);
extern void *idr_remove(struct idr*, int _id);
extern void *idr_find(struct idr*, int _id);

/**
 * @param start 闭区间
 * @param end 闭区间
 */
extern int idr_init_base(struct idr*, uint32_t start, uint32_t end);

static inline int idr16_init(struct idr *idr)
{
	return idr_init_base(idr, 0, S16_MAX - 1);
}

static inline int idr32_init(struct idr *idr)
{
	return idr_init_base(idr, 0, S32_MAX - 1);
}

extern void idr_destroy(struct idr*);

/*
 * TODO ：调用者给出 idr 节点
 */
__END_DECLS

#endif
