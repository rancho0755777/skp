#ifndef __US_XPRT_H__
#define __US_XPRT_H__

#include "../utils/uref.h"
#include "../adt/list.h"
#include "../process/event.h"
#include "types.h"

__BEGIN_DECLS

/* 通过框架来调用 具体实现的回调函数
 * 1. 回调函数中不能进行 xprt 引用计数递减的操作，除非你将它的引用计数递增过
 * 2. 回调函数中不能进行 xprt 销毁操作
 */

struct xprt_operations {
	/**
	 * 主要功能是分配内存和初始化基类的字段
	 * 被动创建的套接字在构造时， opt 会继承侦听套接字，而 user 会是创建它的侦听
	 * 套接字的基类
	 * 同步回调
	 */
	struct xprt* (*constructor)(struct server*, unsigned long opt, void *user);
	/**
	 * 主要功能是释放内存和反初始化基类的字段
	 * 异步/同步回调
	 */
	void (*destructor)(struct xprt*);


	/**
	 * 握手，返回0则握手完成，继续下面的流程，
	 * 返回 -EAGAIN 则重试，否则视为发生错误，关闭底层
	 */
	int (*do_handshake)(struct xprt*, unsigned long event);

	/**
	 * status : 回调的事件 高位为xprt的特有事件，低位为多路复用的状态：
	 *          比如 XPRT_RDREADY | EVENT_READ | EVENT_EOF | EVENT_ERROR ..
	 * 异步回调
	 */
	void (*on_recv)(struct xprt*, unsigned long event);
	/** 
	 * status : 回调的事件 比如 XPRT_WRREADY|EVENT_WRITE|EVENT_EOF|EVENT_ERROR
	 * 异步回调
	 * 如果出现了错误将在触发次函数后，关闭所有事件
	 */
	void (*on_send)(struct xprt*, unsigned long event);
	/** 将在第一传输就绪、数据可读/写时，在 on_recv/on_send 之前调用
	 * 或传输完全被关闭时调用，各种事件只会被触发一次，
	 * 1. 关闭事件必然是最后一次事件，紧接着就是销毁函数的调用
	 * 2. 注意非阻塞连接，必须开启写事件才能在异步连接成功时触发事件
	 * 3. 仅触发了开启事件，才会触发关闭事件
	 * 4. 如果连接被远端拒绝也会触发关闭事件
	 * status : 回调的事件 比如 XPRT_OPENED/XPRT_CLOSED
	 */
	void (*on_changed)(struct xprt*, unsigned long status);

	/*TODO:特定框架的其他钩子函数*/
	/**
	 * 关闭钩子函数，做一些特殊处理
	 * 同步回调，可能会与其他回调同时调用
	 * 所以如果不在事件触发中调用，则必须保证事件已停止
	 */
	void (*on_shutdown)(struct xprt*, int how);
};

/*基类*/
struct xprt {
	/*只读或通过接口操作*/
	unsigned long flags;

	struct uref refs;
	struct list_head node;
	/*默认电平触发*/
	struct uev_stream event;

	struct server *server;
	const struct xprt_operations *xprt_ops;

	/*提供给基类的字段*/
	void *user;
};

#define xprt_ev(xprt) (&(xprt)->event)
#define xprt_fd(xprt) (xprt_ev((xprt))->fd)
#define xprt_type(xprt)		(READ_ONCE((xprt)->flags) & XPRT_TYPE_MASK)
#define xprt_opt(xprt)		(READ_ONCE((xprt)->flags) & XPRT_OPT_MASK)
#define xprt_status(xprt)	(READ_ONCE((xprt)->flags) & XPRT_STATS_MASK)
#define xprt_getcpu(xprt)	(uev_stream_getcpu(xprt_ev((xprt))))
/**
 *  创建不同基础类型的 xprt
 * 1. 根据不同的标识创建不同的继承类传输对象
 * 2. 标识不同，可能需要不同的参数
 * 3. 被动创建的传输对象，不能主动被创建。
 * 4. 创建侦听类的传输对象需要在变参中给定子传输对象的回调函数集合
 * 5. 如果是创建服务器侦听传输对象，变参为被动客户端的操作函数回调集合
 * @param opt 可以是 XPRT_TCPSERV 或 XPRT_TCPCLNT 然后“或”上一些
 * 套接字选项 XPRT_OPT_XXX
 * 或开启事件的选项 XPRT_RDREADY/XPRT_WRREADY
 * !!! 注意，写事件默认是单次触发 !!!
 */
extern struct xprt *create_xprt(struct server *, const struct service_address *,
	unsigned long opt, const struct xprt_operations *, void *user, ...);
/**
 * 强制销毁 xprt
 * 不能在事件回调中调用，否则死锁
 * 并且不会触发 shutdown() 的相关（系统）调用
 * 尽量不要在持有任何原子锁的情况下销毁，因为销毁是可能需要休眠的
 * 销毁后，本路径持有的引用计数也将丢失
 */
extern void destroy_xprt(struct xprt *);
/**
 * 关闭 xprt 读写
 * 语义完全与 shutdown() 一样，会触发 读写的关闭事件
 * 只要你持有 xprt 的引用，你就可以多次关闭它
 */
extern void shutdown_xprt(struct xprt *, int how);

static inline struct xprt *xprt_get(struct xprt *xprt)
{
	if (skp_likely(xprt) && WARN_ON(!uref_get_unless_zero(&xprt->refs)))
		return NULL;
	return xprt;
}

extern void __xprt_put(struct uref *);
static inline void xprt_put(struct xprt *xprt)
{
	if (skp_likely(xprt))
		uref_put(&xprt->refs, __xprt_put);
}

static inline bool xprt_has_closed(const struct xprt *xprt)
{
	unsigned long flags = xprt_status(xprt);
	return (skp_unlikely(flags & XPRT_CLOSED)) ||
		skp_unlikely((flags & XPRT_SHUT_RDWR) == XPRT_SHUT_RDWR);
}

static inline bool xprt_has_shutrd(const struct xprt *xprt)
{
	unsigned long flags = xprt_status(xprt);
	return skp_unlikely(flags & XPRT_SHUTRD) || xprt_has_closed(xprt);
}

static inline bool xprt_has_shutwr(const struct xprt *xprt)
{
	unsigned long flags = xprt_status(xprt);
	return skp_unlikely(flags & XPRT_SHUTWR) || xprt_has_closed(xprt);
}

/*已就绪可以读写*/
static inline bool xprt_is_ready(const struct xprt *xprt)
{
	unsigned long flags = xprt_status(xprt);
	return (skp_likely(flags & XPRT_OPENED)) && !xprt_has_closed(xprt);
}

static inline bool xprt_is_connecting(const struct xprt *xprt)
{
	unsigned long flags = xprt_status(xprt);
	return (flags & XPRT_CONNECTING) && !xprt_has_closed(xprt);
}

/*阻塞连接后，用于设置为非阻塞*/
static inline void xprt_set_nonblock(struct xprt *xprt)
{
	if (!set_fd_nonblock(xprt_fd(xprt)))
		xprt->flags |= XPRT_OPT_NONBLOCK;
}

////////////////////////////////////////////////////////////////////////////////
// 一些辅助函数
////////////////////////////////////////////////////////////////////////////////

static inline ssize_t xprt_read(const struct xprt *x, void *b, size_t s)
{
	ssize_t rc = read(xprt_fd((x)), (b), (s));
	if (skp_unlikely(rc < 0))
		rc = -errno;
	return rc;
}

static inline ssize_t xprt_write(const struct xprt *x, const void *b, size_t s)
{
	ssize_t rc = write(xprt_fd((x)), (b), (s));
	if (skp_unlikely(rc < 0))
		rc = -errno;
	return rc;
}

static inline int xprt_event_add(struct xprt* xprt, uint8_t mask)
{
	if (xprt_has_closed(xprt))
		return -EINVAL;
	if (READ_ONCE(xprt->flags) & XPRT_SHUTRD)
		mask &= ~ EVENT_READ;
	if (READ_ONCE(xprt->flags) & XPRT_SHUTWR)
		mask &= ~ EVENT_WRITE;
	return uev_stream_add(xprt_ev(xprt), mask);
}

static inline int xprt_event_enable(struct xprt *xprt, uint8_t mask)
{
	if (xprt_has_closed(xprt))
		return -EINVAL;
	if (READ_ONCE(xprt->flags) & XPRT_SHUTRD)
		mask &= ~ EVENT_READ;
	if (READ_ONCE(xprt->flags) & XPRT_SHUTWR)
		mask &= ~ EVENT_WRITE;
	return uev_stream_enable(xprt_ev(xprt), mask);
}

#define xprt_event_delete(x) uev_stream_delete_async(xprt_ev((x)))
#define xprt_event_delete_sync(x) uev_stream_delete_sync(xprt_ev((x)))
#define xprt_event_disable(x, mask) uev_stream_disable(xprt_ev((x)), (mask))

////////////////////////////////////////////////////////////////////////////////
// 预实现的二级传输对象
////////////////////////////////////////////////////////////////////////////////

#define xprt_to_tcpserv(__xprt) 										\
	({ struct xprt * __x = (__xprt); skp_likely(__x) ?						\
		container_of((__x), struct xprt_tcpserv, xprt) : NULL;})

#define xprt_is_tcpserv(__x)											\
	(((uint32_t)(READ_ONCE((__x)->flags) & XPRT_TYPE_MASK)) == XPRT_TCPSERV)

#define tcpserv_family(__s)												\
	((__s)->lstn_address.sock_addr.sa_family)

struct xprt_tcpserv {
	/*私有字段，用户只读或通过接口操作*/
	struct xprt xprt; /*base class*/
	union inet_address lstn_address; /*侦听地址*/
	const struct xprt_operations *clnt_xprt_ops;
	/*因为接受了太多被动连接而暂停*/
	bool cool_down;
};

/**
 * 创建服务器
 * 绑定并侦听连接，并在成功后安装
 * xprt 由 tcpserv_ops->constructor() 构造而来的
 */
extern int create_xprt_tcpserv(struct xprt*, struct server *serv,
	const struct service_address *address, unsigned long opt,
	const struct xprt_operations *tcpserv_ops,
	const struct xprt_operations *tcpclnt_ops);

/*关闭服务*/
extern void shutdown_xprt_tcpserv(struct xprt*, int how);

/*接受客户端的连接，如果出错，不会主动关闭侦听对象*/
extern struct xprt *xprt_tcpserv_accept(struct xprt *xprt, unsigned long stats);
/*为侦听套接字预实现的操作，仅需直接使用或包裹使用*/
extern void xprt_tcpserv_recv(struct xprt*, unsigned long);
extern void xprt_tcpserv_send(struct xprt*, unsigned long);
extern void xprt_tcpserv_changed(struct xprt*, unsigned long);
/*如果以 xprt_tcpserv 作为最终继承类，以下为默认构造与析构*/
extern struct xprt *xprt_tcpserv_constructor(struct server*,unsigned long,void*);
extern void xprt_tcpserv_destructor(struct xprt*);
extern const struct xprt_operations xprt_tcpserv_ops;
////////////////////////////////////////////////////////////////////////////////
#define xprt_to_tcpclnt(__xprt)												\
	({ struct xprt * __x = (__xprt); skp_likely(__x) ?						\
		container_of((__x), struct xprt_tcpclnt, xprt) : NULL;})

#define xprt_is_tcpclnt(__xprt)												\
	({ unsigned long __type = READ_ONCE((__xprt)->flags) & XPRT_TYPE_MASK;	\
		__type == XPRT_TCPCLNT || __type == XPRT_TCPTEMP; })

#define tcpclnt_family(__clnt)												\
	({ uint8_t __f1 = (__clnt)->local.sock_addr.sa_family;					\
		uint8_t __f2 = (__clnt)->remote.sock_addr.sa_family;				\
		WARN_ON(__f1 != __f2); __f1; })

struct xprt_tcpclnt {
	/*私有字段，用户只读或通过接口操作*/
	struct xprt xprt;/*base class*/
	struct xprt *lstn_xprt; /**< 指向创建它的侦听套接字的基类*/

	union inet_address local;
	union inet_address remote;

	/*TODO:提供给继承类的字段，由用户初始化，比如控制、套接字选项信息*/
};

/** 创建客户端
 * 发起（非阻塞）连接，并在成功后安装
 */
extern int create_xprt_tcpclnt(struct xprt*, struct server *serv,
	const struct service_address *address, unsigned long opt,
	const struct xprt_operations *tcpclnt_ops);

/*关闭服务*/
extern void shutdown_xprt_tcpclnt(struct xprt*, int how);
/*如果以 xprt_tcpclnt 作为最终继承类，以下为默认构造与析构*/
extern struct xprt *xprt_tcpclnt_constructor(struct server *, unsigned long, void*);
extern void xprt_tcpclnt_destructor(struct xprt*);
////////////////////////////////////////////////////////////////////////////////
// 一些特殊场景可能使用的初始化、反初始化函数
////////////////////////////////////////////////////////////////////////////////
/**初始化一个 tcp 客户端*/
extern void xprt_tcpclnt_init(struct xprt_tcpclnt *, unsigned long flags,
		const struct xprt_operations *);
/** 创建一个替身，除了引用计数，替身将继承原 xprt 的所有状态和资源
 * 必须保证原 src 是有效性，alias 将被初始化，并获得一个引用计数，
 * src 的事件将被删除， alias 不会主动进入事件循环，必须手动的启动
 * 1. 因为有时候应用层的基础 传输对象 被优先创建，当接收到一些数据时，
 * 可能需要创建这个 对象 的子对象来处理后续特殊的业务数据包
 * 2. 尽量在事件回调中调用，否则可能破坏 xprt 的状态机
 * 3. 如果 alias 没有设置回调，则会继承 src 的回调
 * @return false 表示已经被彻底关闭了
 */
#define xprt_tcpclnt_alias(d, s) xprt_tcpclnt_move((d), (s))
/*参考 c++ 的 stl 的移动语义，非线程安全*/
extern bool xprt_tcpclnt_move(struct xprt_tcpclnt *alias, struct xprt_tcpclnt *src);

////////////////////////////////////////////////////////////////////////////////
/// Openssl
/// 由于协议的特殊性，xprt_ssl 应该一直开启读事件
////////////////////////////////////////////////////////////////////////////////

#ifdef ENABLE_SSL

struct ssl_st;
struct ssl_ctx_st;
struct xprt_ssl {
	struct xprt_tcpclnt tcp;

	struct ssl_st *ssl;
	struct ssl_ctx_st *ctx;

	int ev_mask; /*原来的  ev_mask，握手后需要恢复*/
	int handshake_stat; /*XPRT_SSL_XXX*/

	union {
		const char *tlsext_host_name; /*客户端使用*/
		bool (*servername_cb)(struct xprt_ssl *, const char *); /*服务器端使用*/
	};
};

#define xprt_to_ssl(__xprt)												\
({ struct xprt * __x = (__xprt); skp_likely(__x) ?						\
	container_of((__x), struct xprt_ssl, tcp.xprt) : NULL;})

/*环境初始化或内部错误等*/
extern const char *ssl_stack_error(void);
extern void ssl_stack_error_clear(void);
/*
 * 设置默认的cert 和 key
 * 必须保证 cert 和 key 指针的持久有效性
 * 必须要在 xprt_ssl_init() 之前设置
 */
extern int ssl_set_default_certkey(const char *cert, const char *key);

/*获取握手、读写 ssl 的错误*/
extern const char *xprt_ssl_error(const struct xprt_ssl*, int);
/**
 * 构造 一个继承 struct xprt_ssl 用于初始化 xprt_ssl 的字段
 * @param opt 如果不为0，则根据 opt 是客户端还是服务器端 使用默认的上下文
 * @param ctx 如果opt为0，则使用 变参 ctx
 */
extern int xprt_ssl_init(struct xprt_ssl *, unsigned long opt, .../*ctx*/);

/**
 * 设置客户端握手时，提交请求的HostName信息
 * 如果是网络通过代理连接，则需要设置
 */
static inline
void xprt_ssl_set_tlsext_servername(struct xprt_ssl *ssl, const char *host)
{
	ssl->tlsext_host_name = host;
}

/**
 * 设置回调，如果服务器端获取到 HostName 信息则回调
 */
static inline
void xprt_ssl_set_tlsext_servername_cb(struct xprt_ssl *ssl,
				bool (*servername_cb)(struct xprt_ssl *, const char *))
{
	if (WARN_ON(xprt_type(&ssl->tcp.xprt)!=XPRT_TCPTEMP))
		return;
	ssl->servername_cb = servername_cb;
}

/**
 * 析构 一个继承 struct xprt_ssl 用于清理 xprt_ssl 的字段
 */
extern void xprt_ssl_finit(struct xprt_ssl *);
/**
 * 移动语义，非线程安全
 */
extern bool xprt_ssl_move(struct xprt_ssl *alias, struct xprt_ssl *src);

/** 默认关闭 */
extern void xprt_ssl_shutdown(struct xprt *, int how);

/**
 * 默认的握手函数
 * 内部会自动辨别是客户端握手还是服务器端握手
 * 可重入，但不保证线程安全
 */
extern int xprt_ssl_handshake(struct xprt *, unsigned long event);

/*读写*/
extern ssize_t xprt_ssl_read(struct xprt *x, void *b, size_t s);
extern ssize_t xprt_ssl_write(struct xprt *x, const void *b, size_t s);

#endif


__END_DECLS

#endif
