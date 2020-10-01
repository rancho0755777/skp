//
//  socket.h
//  test
//
//  Created by 周凯 on 2018/11/30.
//  Copyright © 2018 zhoukai. All rights reserved.
//

#ifndef __US_SOCKET_H__
#define __US_SOCKET_H__

#include <netdb.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <netinet/in.h>

#include "types.h"

__BEGIN_DECLS

extern struct addrinfo *sock_getaddrinfo(const struct service_address *address,
		int flag, int family, int socktype);

static inline void sock_freeaddrinfo(struct addrinfo *info)
{
	if (skp_likely(info))
		freeaddrinfo(info);
}

typedef void (*sockfd_setopt)(int fd, void *user);
/**
 * 创建侦听套接字
 * @return -1 or sockfd
 */
extern int tcp_listen(const struct service_address *address,
	sockfd_setopt action, void *user);
/**
 * 创建连接套接字
 * @param pflags indicate socket is in connecting
 * @return -1 or sockfd
 */
extern int tcp_connect(const struct service_address *address,
	sockfd_setopt action, void *user, int *pflags, union inet_address *sin);

/**
 * 接受一个连接
 * @param pcfd 不能为空
 * @param cli_addr 获取对端的地址，可以为空
 */
extern int tcp_accept(int lfd, union inet_address *cli_addr);

////////////////////////////////////////////////////////////////////////////////
/// udp_bind/udp_connect 函数中的
/// struct service_address 必须要明确 地址族
/// 因为 udp 不是真正的能连接，"连接"只是命名本地地址和端口
////////////////////////////////////////////////////////////////////////////////

/**
 * 绑定创建 udp 套接字
 * @return 返回套接字或负错误
 */
extern int udp_bind(const struct service_address *baddr);

/**
 * 向固定 地址 发送 udp 包
 * 如果要改变数据目的地址，可以重新连接
 * @return 返回套接字或负错误
 * UDP连接地址表达式必须明确，不能解析为多个32位的地址值
 */
extern int udp_connect(const struct service_address *saddr);

/**
 * 加入一个多播组
 * @param maddr 加入多播组的地址
 * @param local 本地接收网卡(地址)，且必须明确地址族，如果为空则内核选择
 * @return 返回0或负错误
 */
extern int mcast_join(int fd, const struct service_address *maddr,
		const char *local);

/**
 * 设置发送多播数据的本地网卡 (地址)，且必须明确地址族
 * @param id
 */
extern int mcast_set_interface(int fd, const char *iaddr);

///////////////////////////////////////////////////////////////////////////////
//套接字选项快捷设置函数
// @return 返回 0 成功，或返回 负值的 错误号
///////////////////////////////////////////////////////////////////////////////

/** 获取整型套接字选项 */
extern int sockopt_get_intval(int fd, int level, int name, int *val);
/** 设置整型套接字选项 */
extern int sockopt_set_intval(int fd, int level, int name, int val);
/** 获取时间型套接字选项，微秒单位 */
extern int sockopt_get_timeval(int fd, int level, int name, long *usec);
/** 设置时间型套接字选项，微秒单位 */
extern int sockopt_set_timeval(int fd, int level, int name, long usec);

/**
 * 关闭套接字属性
 * @param onoff     是否开启参数：TRUE，开启；FALSE，关闭
 * @param linger    延迟时间参数：
 *   0，发送RST分节给对端，本端无四路终止序列，
 *   即本端端口不会被TIME_WAIT状态保护，可能会被连续使用给
 *   下一个新连接，使下一个连接收到上个连接的旧分节（网络在
 *   途分节）。
 *   > 0（为延迟时间），进程close()套接字时被挂
 *   起（除非套接字被设置为非阻塞模式），直到套接字发送缓冲
 *   区的数据全被对端确认接受，或延迟时间到达（此时内核未将
 *   缓冲区数据没有发送完毕，则close()返回EWOULDBLOCK错
 *   误，且缓冲区数据被丢弃）
 *   单位 10ms ?
 */
extern int sockopt_set_linger(int fd, bool onoff, int linger);

/* 设置套接字的接收低水平位套接字选项
 * 已接收数据至少需要超过此值，可读事件select()才会被唤醒， 默认为1 */
#define sockopt_set_recvlowat(fd, val) \
	(sockopt_set_intval((fd), SOL_SOCKET, SO_RCVLOWAT, (val)))

/* 获取套接字的接收低水平位套接字选项 */
#define sockopt_get_recvlowat(fd) \
	({ int _val = 0; sockopt_get_intval((fd), \
		SOL_SOCKET, SO_RCVLOWAT, &_val); _val; })

/* 设置套接字的发送低水平位套接字选项
 * 发送缓存剩余至少需要超过此值，可写事件select()才会被唤醒，默认为1 */
#define sockopt_set_sendlowat(fd, val) \
	(sockopt_set_intval((fd), SOL_SOCKET, SO_SNDLOWAT, (val)))

/* 获取套接字的发送低水平位套接字选项 */
#define sockopt_get_sendlowat(fd) \
	({ int _val = 0; sockopt_get_intval((fd), \
		SOL_SOCKET, SO_SNDLOWAT, &_val); _val; })

/* 设置套接字的接收缓冲套接字选项 */
#define sockopt_set_recvbuff(fd, val)	\
	(sockopt_set_intval((fd), SOL_SOCKET, SO_RCVBUF, (val)))

/* 获取套接字的接收缓冲套接字选项 */
#define sockopt_get_recvbuff(fd) \
	({ int _val = 0; sockopt_get_intval((fd), \
		SOL_SOCKET, SO_RCVBUF, &_val); _val; })

/* 设置套接字的发送缓冲套接字选项 */
#define sockopt_set_sendbuff(fd, val)	\
	(sockopt_set_intval((fd), SOL_SOCKET, SO_SNDBUF, (val)))

/* 获取套接字的发送缓冲套接字选项 */
#define sockopt_get_sendbuff(fd) \
	({ int _val = 0; sockopt_get_intval((fd), \
		SOL_SOCKET, SO_SNDBUF, &_val); _val; })

/* 开启Nagle算法 */
#define sockopt_enable_nodelay(fd) \
	(sockopt_set_intval((fd), SOL_SOCKET, TCP_NODELAY, 1))

/* 关闭Nagle算法 */
#define sockopt_disable_nodelay(fd) \
	(sockopt_set_intval((fd), SOL_SOCKET, TCP_NODELAY, 0))

/* 得到套接字上的错误值 */
#define sockopt_get_sockerr(fd) \
	({ int _val = 0; sockopt_get_intval((fd), \
		SOL_SOCKET, SO_ERROR, &_val); _val; })

/* 重用绑定地址 */
#define sockopt_reuseaddress(fd) \
	(sockopt_set_intval((fd), SOL_SOCKET, SO_REUSEADDR, 1))

/* 开启广播 */
#define sockopt_enable_broadcast(fd) \
	(sockopt_set_intval((fd), SOL_SOCKET, SO_BROADCAST, 1))

/* 关闭广播 */
#define sockopt_disable_broadcast(fd)	\
	(sockopt_set_intval((fd), SOL_SOCKET, SO_BROADCAST, 0))

/* 通过套接字描述符获取套接字地址族类型
 * ie. SOCK_STREAM/SOCK_DGRAM */
#define sockopt_get_socktype(fd) \
	({ int _val = 0; sockopt_get_intval((fd), \
		SOL_SOCKET, SO_TYPE, &_val); _val; })

#if defined(IP_RECVDSTADDR)

/* 开启接收目的地址 */
  #define sockopt_enable_recvdstaddr(fd) \
	(sockopt_set_intval((fd), IPPROTO_IP, IP_RECVDSTADDR, 1))

/* 关闭接收目的地址 */
  #define sockopt_disable_recvdstaddr(fd) \
	(sockopt_set_intval((fd), IPPROTO_IP, IP_RECVDSTADDR, 0))

#else
  #define sockopt_enable_recvdstaddr(fd) (0)
  #define sockopt_disable_recvdstaddr(fd) (0)
#endif

#if defined(IP_RECVIF)
/* 开启接收接收接口 */
  #define sockopt_enable_recvif(fd) \
	(sockopt_set_intval((fd), IPPROTO_IP, IP_RECVIF, 1))

/* 关闭接收接收接口 */
  #define sockopt_disable_recvif(fd) \
	(sockopt_set_intval((fd), IPPROTO_IP, IP_RECVIF, 0))
#else
  #define sockopt_enable_recvif(fd) (0)
  #define sockopt_disable_recvif(fd) (0)
#endif

/*设置发送超时 毫秒单位*/
#define sockopt_set_sndtimeout(fd, x)	\
	sockopt_set_timeval((fd), SOL_SOCKET, SO_SNDTIMEO, (x) * 1000)

/*设置接收超时 毫秒单位*/
#define sockopt_set_rcvtimeout(fd, x)	\
	sockopt_set_timeval((fd), SOL_SOCKET, SO_RCVTIMEO, (x) * 1000)

/*获取发送超时 毫秒单位*/
#define sockopt_get_sndtimeout(fd) \
	({ long _val = 0; sockopt_get_timeval((fd), \
		SOL_SOCKET, SO_SNDTIMEO, &_val); _val / 1000; })

/*获取接收超时 毫秒单位*/
#define sockopt_get_rcvtimeout(fd) \
	({ long _val = 0; sockopt_get_timeval((fd), \
		SOL_SOCKET, SO_RCVTIMEO, &_val); _val / 1000; })

/**
 * 内核自动使用特殊分节保持探测连接是否还处于存活状态
 * @param idle 在套接字空闲时起，首次探测的时间，单位：秒，如果小于1，则默认为2个小时
 * @param intervel 首次探测后，每次探测的间隔时间，单位：秒，如果小于1，则默认为75秒
 * @param trys 尝试探测的次数，如果小于1，则默认为9次
 * 仅linux支持修改首次探测时间参数
 */
extern int sockopt_enable_keepalive(int fd, int idle, int intervel, int trys);

/*关闭存活自动探测*/
#define sockopt_disable_keepalive(fd) \
	sockopt_set_intval((fd), SOL_SOCKET, SO_KEEPALIVE, 0)

////////////////////////////////////////////////////////////////////////////////
// 套接字地址操作
// 支持 Unix 域套接字的路径转换
// @return 返回 0 成功，或返回 负值的 错误号
////////////////////////////////////////////////////////////////////////////////

extern int sockaddr_ntop(const struct sock_address *n, char *p, size_t );
extern int sockaddr_pton(int family, const char *p, struct sock_address *n);

////////////////////////////////////////////////////////////////////////////////
// 带超时的读写 流类型的 套接字 也可以用于 pipe()
////////////////////////////////////////////////////////////////////////////////

extern ssize_t stream_read(int fd, void *b, size_t l, int *timeout);
extern ssize_t stream_write(int fd, const void *b, size_t l, int *timeout);

////////////////////////////////////////////////////////////////////////////////
// 带超时的读写 数据报类型的 套接字
////////////////////////////////////////////////////////////////////////////////

/**
 * @param srcaddr 如果为空，则不会返回数据报的源地址
 */
extern ssize_t dgram_read(int fd, void *b, size_t l, int *timeout,
		union inet_address *srcaddr);

/**
 * @param dstaddr 如果为空，数据报必须是连接过的
 */
extern ssize_t dgram_write(int fd, const void *b, size_t l, int *timeout,
		const union inet_address *dstaddr);

__END_DECLS

#endif /* socket_h */
