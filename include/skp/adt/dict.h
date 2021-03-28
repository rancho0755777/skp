#ifndef __SU_DICT_H__
#define __SU_DICT_H__

#include "../utils/utils.h"

__BEGIN_DECLS

#define DICT_NODE_POSITION ((void *)0x0000dead)

#ifdef DICT_DEBUG
struct dict_stats {
	uint32_t max_nodes;
	uint32_t avg_nodes;
	uint32_t active_max_nodes;
	uint32_t active_avg_nodes;
	uint32_t rehash_max_nodes;
	uint32_t rehash_avg_nodes;
	uint32_t nr_expands;
	uint32_t nr_reduces;
	uint64_t alloc_mm;
};
#endif

/*哈希节点*/
struct dict;
struct dict_node {
	struct dict_node *next;
	uint32_t hvalue; /*关联数据的哈希值，加速比较操作*/
	union {
		uint32_t flags;
		struct {
			uint32_t whichb:1; /*位于哪个哈希表中*/
			uint32_t bindex:31; /*位于哈希表中的位置*/
		};
	};
};

static inline void dict_node_init(struct dict_node *dhode) /*初始化节点*/
{
	dhode->next = DICT_NODE_POSITION;
	dhode->hvalue = U32_MAX;
	dhode->flags = U32_MAX;
}

struct dict_bucket;
struct dict_table {
	uint32_t nr_nodes; /*表中所有桶中的节点总数*/
	uint32_t size; /*表大小*/
	struct dict_bucket *bucket; /*表（桶）数组起始地址*/
	unsigned long *mapping; /*表中有节点的桶的映射，加速遍历和rehash*/
};

struct dict_ops {
	/*计算哈希回调函数*/
	uint32_t (*calc_hash)(const void *key);
	/* key比较函数，用于插入，查找，删除数据
	 * existed == key return 0 else other return value */
	int (*compare_key)(struct dict_node *existed, const void *key, uint32_t hv);
	/*node比较函数，用于rehash排序*/
	int (*compare_node)(struct dict_node *existed, struct dict_node *insert);
};

struct dict {
	int64_t rehashidx; /*!=-1 表示正在rehash*/
	uint16_t expand_ratio;
	uint16_t reduce_ratio;
	uint32_t init_size;

	struct dict_table *active; /*normal*/
	struct dict_table *rehash; /*rehash*/
	struct dict_table tables[2];

	/*提供给外部使用*/
	void *user_data;
	const struct dict_ops *ops;
#ifdef DICT_DEBUG
	struct dict_stats stats;
#endif
};

struct dict_attr {
	uint16_t expand_ratio;
	uint16_t reduce_ratio;
	uint32_t init_size;
	const struct dict_ops *ops;
};

typedef void (*dict_fn)(struct dict_node *found, void *user_data);

/*初始化*/
extern int dict_init(struct dict *dict, const struct dict_attr *attr);

extern void dict_release(struct dict *dict, dict_fn free, void *user_data);

/*会修改数据，注意一致性*/
extern bool dict_check_rehashing(struct dict *dict);

/*查找*/
extern struct dict_node *
dict_insert(struct dict *dict, const void *key, struct dict_node *node);

/*查找*/
extern struct dict_node *dict_lookup(struct dict *, const void *key);

/*查找移除*/
extern struct dict_node *dict_lookup_remove(struct dict *dict, const void *key);

/*直接移除*/
extern int dict_direct_remove(struct dict *dict, struct dict_node *);

/*非原子操作，返回值仅供参考*/
static inline uint32_t dict_nodes(const struct dict *dict)
{
	return READ_ONCE(dict->active->nr_nodes) +
		(READ_ONCE(dict->rehashidx) > -1 ? READ_ONCE(dict->rehash->nr_nodes):0);
}

extern void dict_stats_print(struct dict *dict);
extern void dict_stats_colloect(struct dict *dict);

///////////////////////////////////////////////////////////////////////////////
// iterator of dict
///////////////////////////////////////////////////////////////////////////////
struct dict_iter {
	struct dict *dict;
	struct dict_node *next;
};

/*返回值，为了遍历而特设的*/
extern struct dict_iter *dict_iter_init(struct dict_iter *, struct dict *);
extern void dict_iter_release(struct dict_iter *);
extern struct dict_node *dict_iter_next(struct dict_iter *);

#define dict_entry(ptr, type, member) \
	container_of(ptr, type, member)

#define dict_for_each(ptr, dict) 											\
	for (struct dict_iter _iter, *_piter = dict_iter_init(&_iter, (dict));	\
			(ptr = dict_iter_next(_piter));									\
			(void)(skp_unlikely(!_piter->next) && ({ dict_iter_release(_piter);	\
				false; })))

#define dict_for_each_entry(ptr, dict, type, member)						\
	for (struct dict_iter _iter, *_piter = dict_iter_init(&_iter, (dict));	\
			({struct dict_node *_ptr = dict_iter_next(_piter); 				\
				_ptr ? ((ptr) = dict_entry(_ptr, type, member)) : NULL; });	\
			(void)(skp_unlikely(!_piter->next) && ({ dict_iter_release(_piter);	\
				false; })))

__END_DECLS

#endif
