//
//  socket.c
//  test
//
//  Created by 周凯 on 2018/11/30.
//  Copyright © 2018 zhoukai. All rights reserved.
//
#include <sys/select.h>
#include <skp/utils/utils.h>
#include <skp/server/socket.h>

#ifndef CONFIG_LISTENQ
# define CONFIG_LISTENQ (1024)
#endif

#undef NEG
#define NEG(x) ((x) > 0 ? -(x) : (x))

int sockopt_get_intval(int fd, int level, int name, int *val)
{
	socklen_t length = sizeof(*val);
	int rc = getsockopt(fd, level, name, val, &length);
	if (skp_unlikely(rc < 0)) {
		log_warn("get socket option [L : %d, O : %d] failed: %s.",
			level, name, strerror_local());
		return -errno;
	}
	return 0;
}

int sockopt_set_intval(int fd, int level, int name, int val)
{
	int rc = setsockopt(fd, level, name, &val, (socklen_t)sizeof(int));
	if (skp_unlikely(rc < 0)) {
		log_warn("set socket option [L : %d, O : %d] failed: %s.",
			level, name, strerror_local());
		return -errno;
	}
	return 0;
}

/**
 * 获取时间型套接字选项
 * 微秒单位
 */
int sockopt_get_timeval(int fd, int level, int name, long *usec)
{
	struct timeval tv;
	socklen_t length = sizeof(tv);
	int rc = getsockopt(fd, level, name, &tv, &length);
	if (skp_unlikely(rc < 0)) {
		log_warn("get socket option [L : %d, O : %d] failed: %s.",
			level, name, strerror_local());
		return -errno;
	} else if (skp_likely(usec)) {
		*usec = tv.tv_sec * 1000000 + tv.tv_usec;
	}

	return 0;
}

/**
 * 设置时间型套接字选项
 * 微秒单位
 */
int sockopt_set_timeval(int fd, int level, int name, long usec)
{
	int rc;
	struct timeval  tv;
	if (usec >= 0) {
		tv.tv_sec = usec / 1000000;
		tv.tv_usec = usec % 1000000;
	} else {
		tv.tv_sec = U32_MAX;
		tv.tv_usec = 0;
	}
	rc = setsockopt(fd, level, name, &tv, (socklen_t)sizeof(tv));
	if (skp_unlikely(rc < 0)) {
		log_warn("set socket option [L : %d, O : %d] failed: %s.",
			level, name, strerror_local());
		return -errno;
	}
	return 0;
}

int sockopt_set_linger(int fd, bool onoff, int linger)
{
	struct linger buff = { onoff ? 1 : 0, linger > 0 ? linger : 0 };
	int rc = setsockopt(fd, SOL_SOCKET, SO_LINGER, &buff, sizeof(buff));
	if (skp_unlikely(rc < 0)) {
		log_warn("set socket option about linger failed: %s.",
			strerror_local());
		return -errno;
	}
	return 0;
}

int sockopt_enable_keepalive(int fd, int idle, int intervel, int trys)
{
	int rc = sockopt_set_intval(fd, SOL_SOCKET, SO_KEEPALIVE, 1);

	if (skp_unlikely(rc))
		return rc;

#ifdef TCP_KEEPIDLE
	if (idle > 0) {
		rc = sockopt_set_intval(fd, IPPROTO_TCP, TCP_KEEPIDLE, idle);
		if (skp_unlikely(rc))
			return rc;
	}
#endif

#ifdef TCP_KEEPINTVL
	if (intervel > 0) {
		rc = sockopt_set_intval(fd, IPPROTO_TCP, TCP_KEEPINTVL, intervel * 2);
		if (skp_unlikely(rc))
			return rc;
	}
#endif

#ifdef TCP_KEEPCNT
	if (trys > 0) {
		rc = sockopt_set_intval(fd, IPPROTO_TCP, TCP_KEEPCNT, trys);
		if (skp_unlikely(rc))
			return rc;
	}
#endif
	return 0;
}

int sockaddr_ntop(const struct sock_address *naddr, char *paddr, size_t len)
{
	const char *ptr;

	BUG_ON(!naddr || !paddr);
	if (skp_unlikely(!len))
		return -EINVAL;

	switch (naddr->sock_addr.sa_family) {
		case AF_INET:
			WARN_ON(naddr->length != sizeof(struct sockaddr_in));
			ptr = inet_ntop(AF_INET, &naddr->sin_addr.sin_addr, paddr, (socklen_t)len);
			break;
		case AF_INET6:
			WARN_ON(naddr->length != sizeof(struct sockaddr_in6));
			ptr = inet_ntop(AF_INET6, &naddr->sin6_addr.sin6_addr, paddr, (socklen_t)len);
			break;
		case AF_UNIX:
			snprintf(paddr, len, "%s", naddr->sock_un.sun_path);
			ptr = paddr;
			break;
		default:
			WARN(1, "unknow type of sockaddr : %u", naddr->sock_addr.sa_family);
			return -EAFNOSUPPORT;
	}

	if (skp_unlikely(!ptr))
		return -errno;
	return 0;
}

int sockaddr_pton(int family, const char *paddr, struct sock_address *naddr)
{
	int rc = 0;
	size_t length = 0;
	BUG_ON(!naddr || !paddr);

	switch (family) {
		case AF_INET:
			length = sizeof(struct sockaddr_in);
			rc = inet_pton(AF_INET, paddr, &naddr->sin_addr.sin_addr);
			break;
		case AF_INET6:
			length = sizeof(struct sockaddr_in6);
			rc = inet_pton(AF_INET6, paddr, &naddr->sin6_addr.sin6_addr);
			break;
		case AF_UNIX:
			length = sizeof(naddr->sock_un.sun_path);
			length = snprintf(naddr->sock_un.sun_path, length, "%s", paddr);
			rc = 0;
			break;
		default:
			WARN(1, "unknow type of sockaddr : %u", family);
			return -EAFNOSUPPORT;
	}

	if (skp_unlikely(rc == 0)) {
		return -EINVAL;
	} else if (skp_unlikely(rc < 0)) {
		return -errno;
	}

	naddr->length = (uint32_t)length;
	naddr->sock_addr.sa_family = family;
	return 0;
}

static const char *sock_gaistrerror(int error)
{
	switch (error)
	{
#ifdef EAI_ADDRFAMILY
		case NEG(EAI_ADDRFAMILY):
			return "address family for host not supported";
#endif
		case NEG(EAI_AGAIN):
			return "temporary failure in name resolution";

		case NEG(EAI_BADFLAGS):
			return "invalid flags value";

		case NEG(EAI_FAIL):
			return "non-recoverable failure in name resolution";

		case NEG(EAI_FAMILY):
			return "address family not supported";

		case NEG(EAI_MEMORY):
			return "memory allocation failure";

#ifdef EAI_NODATA
		case NEG(EAI_NODATA):
			return "no address associated with host";
#endif
		case NEG(EAI_NONAME):
			return "host nor service provided, or not known";

		case NEG(EAI_SERVICE):
			return "service not supported for socket type";

		case NEG(EAI_SOCKTYPE):
			return "socket type not supported";

		case NEG(EAI_SYSTEM):
			return "system error";

		default:
			return "unknown getaddrinfo() error";
	}
}

struct addrinfo *sock_getaddrinfo(const struct service_address *address,
		int flag, int family, int socktype)
{
	int n = 0;
	struct addrinfo hints;
	struct addrinfo *addrinfo = NULL;

	if (skp_unlikely(!address || !address->host || !address->serv))
		return NULL;

	memset(&hints, 0, sizeof(hints));

	hints.ai_flags = AI_CANONNAME | flag;
	hints.ai_family = family;
	hints.ai_socktype = socktype;

again:
	n = getaddrinfo(address->host ?:"localhost", address->serv?:"8080",
			&hints, &addrinfo);

	if (skp_unlikely(n != 0)) {
		if (skp_likely(n == EAI_AGAIN))
			goto again;
		log_warn("Can't get address information "
			"about socket by [%s : %s] : %s.",
			address->host?:"localhost", address->serv?:"8080",
			sock_gaistrerror(NEG(n)));
		addrinfo = NULL;
	}

	return addrinfo;
}

int tcp_listen(const struct service_address *addr, sockfd_setopt action,
		void *user)
{
	struct addrinfo *ai;
	int sfd = -EINVAL, rc = 0;

	ai = sock_getaddrinfo(addr, AI_PASSIVE, AF_UNSPEC, SOCK_STREAM);
	if (skp_unlikely(!ai))
		return -EINVAL;

	for (struct addrinfo *tmp = ai; tmp; tmp = tmp->ai_next) {
		sfd = socket(tmp->ai_family, tmp->ai_socktype, tmp->ai_protocol);
		if (skp_unlikely(sfd < 0)) {
			rc = -errno;
			continue;
		}
		sockopt_reuseaddress(sfd);
		if (action)
			action(sfd, user);
		rc = bind(sfd, tmp->ai_addr, tmp->ai_addrlen);
		if (skp_likely(!rc)) {
			rc = listen(sfd, CONFIG_LISTENQ);
			if (skp_likely(!rc))
				break;
		}
		rc = -errno;
		close(sfd);
	}

	sock_freeaddrinfo(ai);
	if (skp_likely(!rc))
		return sfd;
	log_warn("listen on %s:%s failed : %s", addr->host, addr->serv,
		strerror_local());
	return rc;
}

int tcp_connect(const struct service_address *address, sockfd_setopt action,
		void *user, int *pflags, union inet_address *pinetaddr)
{
	struct addrinfo *ai;
	union inet_address inetaddr;
	int flags = 0, sfd = -EINVAL, rc = 0;

	ai = sock_getaddrinfo(address, 0, AF_UNSPEC, SOCK_STREAM);
	if (skp_unlikely(!ai))
		return -EINVAL;

	if (skp_unlikely(!pflags))
		pflags = &flags;
	if (skp_unlikely(!pinetaddr))
		pinetaddr= &inetaddr;

	for (struct addrinfo *tmp = ai; tmp; tmp = tmp->ai_next) {
		sfd = socket(tmp->ai_family, tmp->ai_socktype, tmp->ai_protocol);
		if (skp_unlikely(sfd < 0)) {
			rc = -errno;
			continue;
		}
		if (action)
			action(sfd, user);

		fill_inet_address(pinetaddr, tmp->ai_addr, tmp->ai_addrlen);
		rc = connect(sfd, tmp->ai_addr, tmp->ai_addrlen);
		if (skp_likely(!rc)) {
			*pflags = 0;
			break;
		} else if (skp_likely(errno == EINPROGRESS)) {
			/*防止冲去errno的存储的错误值*/
			*pflags = EINPROGRESS;
			rc = 0;
			break;
		}

		rc = -errno;
		close(sfd);
	}

	sock_freeaddrinfo(ai);
	if (skp_likely(!rc))
		return sfd;
	log_warn("connecting [%s:%s] address failed : %s",
		address->host, address->serv, strerror_local());
	return rc;
}

int tcp_accept(int lfd, union inet_address *cli_addr)
{
	int rc;
	union inet_address addr;
	socklen_t length = sizeof(addr);

	int cfd = accept(lfd, &addr.sock_addr, &length);
	if (skp_likely(cfd > -1)) {
		if (cli_addr)
			*cli_addr = addr;
		return cfd;
	}

	rc = errno;
	if (skp_likely(rc == EAGAIN || rc == EMFILE || rc == ECONNABORTED)) {
		/*打开的文件太多了，需要关闭读事件，暂停 accept() */
		if (WARN_ON(rc == EMFILE)) {
			log_warn("accept filed : %s", __strerror_local(rc));
		} else {
			/*均当作重试处理*/
			rc = errno = EAGAIN;
		}
	}
	return -rc;
}


int udp_bind(const struct service_address *address)
{
	struct addrinfo *ai;
	int rc = 0, fd = -EINVAL;

	ai = sock_getaddrinfo(address, 0, AF_UNSPEC, SOCK_DGRAM);
	if (skp_unlikely(!ai))
		return -EINVAL;

	for (struct addrinfo *tmp = ai; tmp; tmp = tmp->ai_next) {
		fd = socket(tmp->ai_family, tmp->ai_socktype, tmp->ai_protocol);
		if (skp_unlikely(fd < 0)) {
			rc = -errno;
			continue;
		}
		sockopt_reuseaddress(fd);
		rc = bind(fd, tmp->ai_addr, tmp->ai_addrlen);
		if (skp_likely(!rc))
			break;
		rc = -errno;
		close(fd);
		fd = -EINVAL;
	}

	sock_freeaddrinfo(ai);
	if (skp_likely(!rc))
		return fd;
	log_warn("bind [%s:%s] address failed : %s",
		address->host, address->serv, strerror_local());
	return rc;
}

int udp_connect(const struct service_address *address)
{
	struct addrinfo *ai;
	int rc = 0, fd = -EINVAL;

	ai = sock_getaddrinfo(address, 0, AF_UNSPEC, SOCK_DGRAM);
	if (skp_unlikely(!ai))
		return -EINVAL;

	if (skp_unlikely(ai->ai_next)) {
		rc = -EINVAL;
		goto out;
	}

	fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
	if (skp_unlikely(fd < 0)) {
		rc = -errno;
		goto out;
	}

	rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
	if (skp_unlikely(rc)) {
		rc = -errno;
		close(fd);
		fd = -EINVAL;
	}

out:
	sock_freeaddrinfo(ai);
	if (skp_likely(!rc))
		return fd;
	log_warn("bind [%s:%s] address failed : %s",
		address->host, address->serv, strerror_local());
	return rc;
}

static int __mcast_join(int fd, struct addrinfo *maddr, struct addrinfo *laddr)
{
	struct ip_mreq   mreqv4;
    struct ipv6_mreq mreqv6;
	char *optval = NULL;
	int optlevel, option, optlen, rc;

	if (skp_unlikely(maddr->ai_family != laddr->ai_family))
		return -EINVAL;

	if (maddr->ai_family == AF_INET) {
		optlevel = IPPROTO_IP;
		option = IP_ADD_MEMBERSHIP;
		optval = (char *)& mreqv4;
		optlen = sizeof(mreqv4);
		mreqv4.imr_multiaddr.s_addr =
			((struct sockaddr_in *)maddr->ai_addr)->sin_addr.s_addr;
		mreqv4.imr_interface.s_addr =
			((struct sockaddr_in *)laddr->ai_addr)->sin_addr.s_addr;
	} else if (maddr->ai_family == AF_INET6) {
		optlevel = IPPROTO_IPV6;
		option = IPV6_JOIN_GROUP;
		optval = (char *) &mreqv6;
		optlen = sizeof(mreqv6);
		mreqv6.ipv6mr_multiaddr =
			((struct sockaddr_in6 *)maddr->ai_addr)->sin6_addr;
		/*TODO:可能又问题*/
		mreqv6.ipv6mr_interface =
			((struct sockaddr_in6 *)laddr->ai_addr)->sin6_scope_id;
	} else {
		return -EAFNOSUPPORT;
	}

	rc = setsockopt(fd, optlevel, option, optval, optlen);
	if (skp_unlikely(rc))
		rc = -errno;

	return rc;
}

int mcast_join(int fd, const struct service_address *maddr, const char *local)
{
	int nr = 0, rc = 0;
	struct service_address laddr;
	struct addrinfo *mcast_ai = NULL;
	struct addrinfo *local_ai = NULL;

	rc = -EINVAL;
	mcast_ai = sock_getaddrinfo(maddr, 0, AF_UNSPEC, SOCK_DGRAM);
	if (skp_unlikely(!mcast_ai))
		goto out;

	for (struct addrinfo *tmp = mcast_ai; tmp; tmp = tmp->ai_next) {
		rc = -EINVAL;
		laddr.serv = "";
		laddr.host = local?:"0.0.0.0";
		local_ai = sock_getaddrinfo(&laddr,0,tmp->ai_family,tmp->ai_socktype);
		if (skp_unlikely(!local_ai))
			continue;

		if (skp_likely(!local_ai->ai_next)) {
			rc = __mcast_join(fd, tmp, local_ai);
			if (skp_likely(!rc))
				nr++;
		}

		sock_freeaddrinfo(local_ai);
		local_ai = NULL;
	}

out:
	sock_freeaddrinfo(mcast_ai);
	sock_freeaddrinfo(local_ai);

	/*只要加入成功一个就算返回成功指示*/
	return nr?0:rc;
}

static int __mcast_set_interface(int fd, struct addrinfo *laddr)
{
	char *optval = NULL;
	int optlevel, option, optlen, rc;

	if (laddr->ai_family == AF_INET) {
		optlevel = IPPROTO_IP;
		option = IP_MULTICAST_IF;
		optval = (char *)&((struct sockaddr_in *)laddr->ai_addr)->sin_addr.s_addr;
		optlen = sizeof(((struct sockaddr_in *)laddr->ai_addr)->sin_addr.s_addr);
	} else if (laddr->ai_family == AF_INET6) {
		optlevel = IPPROTO_IPV6;
		option = IPV6_MULTICAST_IF;
		optval = (char *)&((struct sockaddr_in6 *)laddr->ai_addr)->sin6_scope_id;
		optlen = sizeof(((struct sockaddr_in6 *)laddr->ai_addr)->sin6_scope_id);
	} else {
		rc = errno = EAFNOSUPPORT;
		return -rc;
	}

	rc = setsockopt(fd, optlevel, option, optval, optlen);
	if (skp_unlikely(rc))
		rc = -errno;

	return rc;
}

int mcast_set_interface(int fd, const char *local)
{
	int rc = -EINVAL;
	struct service_address laddr;
	struct addrinfo *local_ai = NULL;

	laddr.serv = "";
	laddr.host = local?:"0.0.0.0";
	local_ai = sock_getaddrinfo(&laddr,0, AF_UNSPEC, SOCK_DGRAM);
	if (skp_unlikely(!local_ai))
		return -EINVAL;

	if (skp_likely(!local_ai->ai_next))
		rc = __mcast_set_interface(fd, local_ai);

	sock_freeaddrinfo(local_ai);
	return rc;
}

static int check_channel(int fd, int *ptimeout, bool r)
{
	int rc;
	fd_set fdset;
	struct timeval tv;
	cycles_t start = 0;
	int timeout = *ptimeout;

	FD_ZERO(&fdset);
	FD_SET(fd, &fdset);

	if (timeout < 0) {
		timeout = U32_MAX;
	} else {
		start = get_cycles();
	}

	tv.tv_sec = timeout/1000;
	tv.tv_usec = (timeout%1000)*1000;

	rc = select(fd+1, r?&fdset:0, (!r)?&fdset:0, 0, &tv);
	if (skp_unlikely(rc < 0)) {
		if (errno == EINTR) {
			rc = 0;
			goto timedout;
		}
		return -errno;
	}

timedout:
	if (start) {
		timeout -= (int)(cycles_to_ns(get_cycles() - start)/1000000);
		if (timeout < 1)
			timeout = 0;
		*ptimeout = timeout;
	}

	return rc?0:-ETIMEDOUT;
}

ssize_t stream_read(int fd, void *b, size_t l, int *ptimeout)
{
	ssize_t rc;
	char *buff = b;
	int timedout = S32_MAX;

	if (skp_unlikely(fd < 0 || !l))
		return -EINVAL;

	while (l > 0) {
		rc = check_channel(fd, ptimeout?:&timedout, true);
		if (skp_likely(!rc)) {
			rc = read(fd, buff, l);
			if (skp_unlikely(rc < 0)) {
				return -errno;
			} else if (!rc) {
				return buff - (char*)b;
			}
			buff+=l;
			l-=rc;
		} else {
			if (rc != -ETIMEDOUT)
				return rc;
			/*中断或超时*/
			break;
		}
	}

	rc = buff - (char*)b;

	return rc?:-ETIMEDOUT;
}

ssize_t stream_write(int fd, const void *b, size_t l, int *ptimeout)
{
	ssize_t rc;
	const char *buff = b;
	int timedout = S32_MAX;

	if (skp_unlikely(fd < 0 || !l))
		return -EINVAL;

	while (l > 0) {
		rc = check_channel(fd, ptimeout?:&timedout, false);
		if (skp_likely(!rc)) {
			rc = write(fd, buff, l);
			if (skp_unlikely(rc < 0)) {
				return -errno;
			} else if (!rc) {
				return buff - (const char*)b;
			}
			buff+=l;
			l-=rc;
		} else {
			if (rc != -ETIMEDOUT)
				return rc;
			/*中断或超时*/
			break;
		}
	}

	rc = buff - (const char*)b;

	return rc?:-ETIMEDOUT;
}

ssize_t dgram_read(int fd, void *b, size_t l, int *ptimeout,
		union inet_address *srcaddr)
{
	ssize_t rc;
	int timedout = S32_MAX;
	socklen_t sl = sizeof(*srcaddr);

	if (skp_unlikely(fd < 0 || !l))
		return -EINVAL;
	rc = check_channel(fd, ptimeout?:&timedout, true);
	if (skp_likely(!rc)) {
		rc = recvfrom(fd, b, l, 0, srcaddr?&srcaddr->sock_addr:NULL,
				srcaddr?&sl:NULL);
		if (skp_unlikely(rc < 0))
			return -errno;
	}

	return rc;
}

ssize_t dgram_write(int fd, const void *b, size_t l, int *ptimeout,
		const union inet_address *dstaddr)
{
	ssize_t rc;
	int timedout = S32_MAX;
	socklen_t sl = dstaddr?(dstaddr->sock_addr.sa_family==AF_INET?
		sizeof(dstaddr->sin_addr):sizeof(dstaddr->sin6_addr)):0;

	if (skp_unlikely(fd < 0 || !l))
		return -EINVAL;
	rc = check_channel(fd, ptimeout?:&timedout, false);
	if (skp_likely(!rc)) {
		rc = sendto(fd, b, l, 0, dstaddr?&dstaddr->sock_addr:NULL, sl);
		if (skp_unlikely(rc < 0))
			return -errno;
	}

	return rc;
}
