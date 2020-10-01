#ifndef __US_SERVER_H__
#define __US_SERVER_H__

#include "../utils/uref.h"
#include "../utils/atomic.h"
#include "../utils/spinlock.h"
#include "../adt/list.h"
#include "../process/wait.h"
#include "../process/workqueue.h"
#include "types.h"

__BEGIN_DECLS

/*
 * 服务器对象主要管理所有传输对象，方便优雅停止服务
 * 它仅支持简单的继承，server 对象必须位于结构体的第一个字段
 * 传递 继承类的结构体大小给 metasize 参数
 * 框架帮助分配内存，因 server 对象数量肯定不多，没有必要使用内存池，
 * 之后由用户初始化 非 server 的字段
 * 并根据需要 设置 destructor() 回调，在销毁 server 时
 * 回调此函数进行 非 server 字段的销毁工作
 */

struct server {
	/*私有字段只读或通过API进行操作*/
	spinlock_t lock;
	uint32_t flags;
	struct uref refs;

	uint32_t max_xprts;
	uint32_t nr_xprts;
	struct list_head xprt_list;
	wait_queue_head_t waitqueue;
	/*other statistic*/

	/*提供给用户的字段*/
	void (*destructor)(struct server*);

};

extern struct server *___alloc_server(size_t metasize, uint32_t max_xprts,
	uint32_t opt);

extern void destroy_server(struct server *server);

extern void server_loop(struct server *);

extern void __server_stop(struct server *, bool destroy_xprts);

/*关闭所有连接*/
static inline void server_stop(struct server * serv)
{
	__server_stop(serv, true);
}

/*不关闭连接，仅退出主循环*/
static inline void server_pause(struct server * serv)
{
	__server_stop(serv, false);
}

static inline void __wake_up_server(struct server *serv)
{
	if (waitqueue_active(&serv->waitqueue))
		wake_up_all_locked(&serv->waitqueue);
}

static inline uint32_t server_get_stats(const struct server *serv)
{
	return READ_ONCE(serv->flags) & SERVER_STATS_MASK;
}

static inline uint32_t server_set_stats(struct server *serv, uint32_t stats)
{
	uint32_t opt = READ_ONCE(serv->flags) & ~ SERVER_STATS_MASK;
	uint32_t old = READ_ONCE(serv->flags) & SERVER_STATS_MASK;
	serv->flags = opt | (stats & SERVER_STATS_MASK);
	return old;
}

static inline bool __server_has_stopped(const struct server *serv)
{
	uint32_t stats = server_get_stats(serv);
	return skp_unlikely(stats == SERVER_STOPPING || stats == SERVER_STOPPED ||
		stats == SERVER_DESTROYED);
}

static inline bool __server_has_fulled(struct server *serv)
{
	return skp_unlikely(serv->nr_xprts >= serv->max_xprts);
}

static inline void server_get(struct server *serv)
{
	BUG_ON(!serv || !uref_get_unless_zero(&serv->refs));
}

static inline void __server_put(struct server *serv)
{
	BUG_ON(__uref_put(&serv->refs));
	__wake_up_server(serv);
}

static inline void server_put(struct server *serv)
{
	spin_lock(&serv->lock);
	__server_put(serv);
	spin_unlock(&serv->lock);
}

static inline bool server_has_stopped(struct server *serv)
{
	bool stopped;
	spin_lock(&serv->lock);
	stopped = __server_has_stopped(serv);
	spin_unlock(&serv->lock);
	return stopped;
}

__END_DECLS

#endif
