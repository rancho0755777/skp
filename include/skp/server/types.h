#ifndef __US_SERVER_TYPES_H__
#define __US_SERVER_TYPES_H__

#include <sys/socket.h>
#include <sys/fcntl.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "../utils/utils.h"

__BEGIN_DECLS

struct xprt;
struct server;
typedef int socket_t;

/*server.flags*/
enum {
	/*使用锁保护，状态是互斥的*/
	SERVER_INITING = 1,
	SERVER_RUNNING = 2,
	SERVER_STOPPING = 3,
	SERVER_STOPPED = 4,
	SERVER_DESTROYED = 5,
	SERVER_STATS_MASK = 0x0000000fU,

	/*other option*/
};

enum {
	/*使用 cmpchg保护，类型是互斥的*/
	/*type of xprt*/
	XPRT_TCPSERV = 1,
	XPRT_TCPCLNT = 2,
	XPRT_TCPTEMP = 3, /**< passive client xprt, create by tcpserv*/
	XPRT_TYPE_MASK = 0x0000000fU, /**< 可以支持15种基础类型的传输对象*/

	/*option for creating xprt*/
	XPRT_OPT_NONBLOCK     = 0x00000010U, /**< 非阻塞IO（包括connect()与accept()）*/
	XPRT_OPT_TCPNAGLEOFF  = 0x00000020U, /**< 关闭小包延迟算法*/
	XPRT_OPT_TCPKEEPALIVE = 0x00000040U, /**< 通过特殊分节自动保持活动链接*/
	XPRT_OPT_TCPLINGEROFF = 0x00000080U, /**< 没有四路挥手关闭*/
	XPRT_OPT_TCPLARGELINGER = 0x00000100U, /**< 无限延迟关闭，直到发送完毕，默认 81920 毫秒*/
	XPRT_OPT_MASK = 0x0000fff0U,

	/* status bit of xprt
	 * 首次接收数据和完全关闭作为独立的事件，用 on_changed() 回调响应
	 */
	/*0*/XPRT_ATTACHED_BIT = ilog2(XPRT_TYPE_MASK + XPRT_OPT_MASK + 1), /**< 已经附加在管理容器中*/
	/*1*/XPRT_CONNECTING_BIT,  /**< 正在连接*/
	/*2*/XPRT_OPENED_BIT, /**< 第一次触发打开事件，变为可读/可写*/
	/*3*/XPRT_CLOSED_BIT, /**< 第一次触发关闭事件，完全关闭，此后一定不会再次触发任何事件回调，
	                        *  在强制关闭时，可以无 opened 状态下触发 closed */
	/*4*/XPRT_SHUTWR_BIT, /**< 已经执行写关闭, 或发送了fin分节*/
	/*5*/XPRT_SHUTRD_BIT, /**< 已经执行读关闭, 或接收了fin分节*/
	/*6*/XPRT_RDREADY_BIT, /**< 读就绪，仅为事件标识，不驻留在状态中*/
	/*7*/XPRT_WRREADY_BIT, /**< 写就绪，仅为事件标识，不驻留在状态中*/
	/*8*/XPRT_CONNREFUSED_BIT, /**< 连接拒绝，作为最后退出状态*/
	/*9*/XPRT_HANDSHAKED_BIT, /**< 已完成握手*/

	XPRT_STATS_MASK = (~(XPRT_TYPE_MASK | XPRT_OPT_MASK)),

	XPRT_ATTACHED = 1U <<XPRT_ATTACHED_BIT,
	XPRT_CONNECTING = 1U << XPRT_CONNECTING_BIT,
	XPRT_OPENED = 1U << XPRT_OPENED_BIT,
	XPRT_CLOSED = 1U << XPRT_CLOSED_BIT,
	XPRT_SHUTWR = 1U << XPRT_SHUTWR_BIT,
	XPRT_SHUTRD = 1U << XPRT_SHUTRD_BIT,
	XPRT_RDREADY = 1U << XPRT_RDREADY_BIT,
	XPRT_WRREADY = 1U << XPRT_WRREADY_BIT,
	XPRT_CONNREFUSED = 1U << XPRT_CONNREFUSED_BIT,
	XPRT_HANDSHAKED = 1U << XPRT_HANDSHAKED_BIT,

	XPRT_SHUT_RDWR = XPRT_SHUTWR | XPRT_SHUTRD,

#ifdef ENABLE_SSL
	XPRT_SSL_NONHANDSHAKE = 0,
	XPRT_SSL_HANDSHAKING,
	XPRT_SSL_HANDSHAKED,
#endif

};

#ifdef INET6_ADDRSTRLEN
# define INET_ADDRESS_STRLEN (INET6_ADDRSTRLEN + 1)
#else
# define INET_ADDRESS_STRLEN 64
#endif

#define INET_PORT_STRLEN sizeof("65536")

/*为了节省内存和实现协议无关，定义Intelnet地址*/
union inet_address {
	struct sockaddr sock_addr;
	struct sockaddr_in sin_addr;
	struct sockaddr_in6 sin6_addr;
};

static inline uint32_t inet_address_port(const union inet_address*iaddr)
{
	return iaddr->sock_addr.sa_family == AF_INET ?
		ntohs(iaddr->sin_addr.sin_port) : ntohs(iaddr->sin6_addr.sin6_port);
}

/*unix域地址*/
union unix_address {
	struct sockaddr sock_addr;
	struct sockaddr_un sock_un;
};

/*全类型套接字地址，主要在栈内和进行地址转换时使用*/
struct sock_address {
	union {
		struct sockaddr_storage storage;
		struct sockaddr sock_addr;
		struct sockaddr_un sock_un;
		struct sockaddr_in sin_addr;
		struct sockaddr_in6 sin6_addr;
	};
	socklen_t length;
};

/*实现协议无关时传递主机名和服务名*/
struct service_address {
	const char *host;
	const char *serv;
};

static __always_inline void fill_inet_address( union inet_address *dst,
		const struct sockaddr *src, socklen_t length)
{
	memcpy(&dst->sock_addr, src, length);
}

static __always_inline void fill_sock_address(struct sock_address *dst,
		const struct sockaddr *src, socklen_t length)
{
	dst->length = length;
	memcpy(&dst->sock_addr, src, length);
}

static __always_inline void copy_sock_address(struct sock_address *dst,
		const struct sock_address *src)
{
	dst->length = src->length;
	if (skp_likely(dst->length)) {
		memcpy(&dst->sock_addr, &src->sock_addr, src->length);
	} else {
		memset(dst, 0, sizeof(*dst));
	}
}

static __always_inline size_t inet_address2sock(const union inet_address *iaddr,
	struct sock_address *saddr)
{
	int fa = iaddr->sock_addr.sa_family;
	saddr->length = fa == AF_INET ? sizeof(struct sockaddr_in) :
		(fa == AF_INET6 ? sizeof(struct sockaddr_in6) : 0);
	if (WARN_ON(!saddr->length))
		return 0;
	memcpy(&saddr->sock_addr, &iaddr->sock_addr, saddr->length);
	return saddr->length;
}

static __always_inline size_t sock_address2inet(const struct sock_address *saddr,
	union inet_address *iaddr)
{
	int fa = saddr->sock_addr.sa_family;
	size_t length = fa == AF_INET ? sizeof(struct sockaddr_in) :
		(fa == AF_INET6 ? sizeof(struct sockaddr_in6) : 0);
	if (WARN_ON(length != saddr->length))
		return 0;
	memcpy(&iaddr->sock_addr, &saddr->sock_addr, length);
	return length;
}

__END_DECLS

#endif
