#include <stdarg.h>
#include <skp/server/xprt.h>
#include <skp/server/server.h>
#include <skp/server/socket.h>
#include <skp/process/event.h>
#include <skp/process/thread.h>
#include <skp/mm/slab.h>

#ifndef CONFIG_MAX_ACCEPTS
# ifdef __apple__
#  define CONFIG_MAX_ACCEPTS (16)
# else
#  define CONFIG_MAX_ACCEPTS (256)
# endif
#endif

#define XPRT_DEBUG

#ifdef XPRT_DEBUG
# define XPRT_BUG_ON(x) BUG_ON((x))
# define XPRT_WARN_ON(x) WARN_ON((x))
# define LOG_XPRT_CHANGED_BIT(xprt, bits)						\
	log_info("parallel change status of xprt "					\
		"[%p]/[0x%lx] : expected change [0x%lx]",				\
		(xprt), (xprt)->flags, (unsigned long)bits)
# define LOG_XPRT_CHANGED_EVENT(xprt, ev, op)					\
	log_info("change [" #op "] event of xprt "					\
		"[%p]/[0x%lx] failed : expected change [0x%x]",			\
		(xprt), ((xprt)->flags) >> XPRT_ATTACHED_BIT, ev)

static inline int xprt_opt_check(const struct xprt_operations *opt)
{
	if (WARN_ON(!opt || !opt->constructor || !opt->destructor ||
			(!opt->on_recv && !opt->on_send)))
		return -EINVAL;
	return 0;
}

static inline int service_address_check(const struct service_address *address)
{
	if (WARN_ON(!address || !address->host || !address->serv))
		return -EINVAL;
	return 0;
}

static inline int create_xprt_check(struct server *serv,
	const struct service_address *address, unsigned long opt,
	const struct xprt_operations *lstn_ops)
{
	if (xprt_opt_check(lstn_ops))
		return -EINVAL;
	if (service_address_check(address))
		return -EINVAL;
	if (WARN_ON(!serv || (opt & XPRT_STATS_MASK)))
		return -EINVAL;
	return 0;
}
#else
# define XPRT_BUG_ON(x) ((void)(x))
# define XPRT_WARN_ON(x) ((void)(x))
# define LOG_XPRT_CHANGED_BIT(xprt, bits)
# define LOG_XPRT_CHANGED_EVENT(xprt, ev, op)
# define xprt_opt_check(x) (0)
# define service_address_check(x) (0)
# define create_xprt_check(x, ...) (0)
#endif

static inline bool xprt_flags_closed(unsigned long flags)
{
	return ((flags&XPRT_CLOSED))||((flags&XPRT_SHUT_RDWR)==XPRT_SHUT_RDWR);
}

static inline bool xprt_flags_detached(unsigned long flags)
{
	return !(flags & XPRT_ATTACHED);
}

struct xprt_rcu {
	struct xprt *xprt;
	struct rcu_head rcu;
};

/*non return call*/
#define xprt_ops_call(x, cb, ...)							\
do { 														\
	struct xprt *__xprt = (struct xprt*)(x);				\
	if (skp_likely(__xprt->xprt_ops->cb))					\
		__xprt->xprt_ops->cb(__xprt, ##__VA_ARGS__);		\
} while (0)


#define xprt_ops_call_ret(x, cb, ...)						\
do { 														\
	struct xprt *__xprt = (struct xprt*)(x);				\
	if (skp_likely(__xprt->xprt_ops->cb))					\
		return __xprt->xprt_ops->cb(__xprt, ##__VA_ARGS__);	\
	return 0;												\
} while (0)

static void process_xprt_event(struct uev_stream *ev, uint16_t mask);

/**
 * 安装传输对象，服务器对象管理所有的传输对象，并持有一个引用计数
 */
static int attach_xprt(struct xprt *xprt, int fd, unsigned long flags,
	struct server *serv, const struct xprt_operations *ops)
{
	int rc = 0;

	/*添加到管理容器的动作第一置于最后，方便错误回滚*/
	server_get(serv);

	/*初始化一些字段*/
	xprt->flags = flags;
	xprt->server = serv;
	xprt->xprt_ops = ops;
	uref_init(&xprt->refs);
	INIT_LIST_HEAD(&xprt->node);

	/*初始化事件*/
	uev_stream_init(xprt_ev(xprt), fd, process_xprt_event);

	/*TODO:处理停止状态*/
	spin_lock(&serv->lock);
	if (!__server_has_fulled(serv)) {
		serv->nr_xprts++;
		/*增加引用计数，防止异步释放 xprt*/
		if (xprt_is_tcpserv(xprt_get(xprt))) {
			/*方便优先杀掉侦听套接字，终止外部新连接*/
			list_add(&xprt->node , &serv->xprt_list);
		} else {
			list_add_tail(&xprt->node, &serv->xprt_list);
		}
		XPRT_BUG_ON(__test_and_set_bit(XPRT_ATTACHED_BIT, &xprt->flags));
	} else {
		rc = -EMFILE;
		__server_put(xchg_ptr(&xprt->server, NULL));
	}
	spin_unlock(&serv->lock);

	/*尚未初始化完成，不能在此启动事件*/

	return rc;
}

static bool detach_xprt(struct xprt *xprt)
{
	struct server *serv = READ_ONCE(xprt->server);

	if (skp_unlikely(!serv))
		return false;
	if (!test_and_clear_bit(XPRT_ATTACHED_BIT, &xprt->flags))
		return false;

	spin_lock(&serv->lock);
	XPRT_BUG_ON(xprt->server != serv);
	XPRT_BUG_ON(list_empty(&xprt->node));
	serv->nr_xprts--;
	list_del_init(&xprt->node);
	spin_unlock(&serv->lock);
	return true;
}

static inline void barrier_xprt(struct xprt *xprt)
{
	struct server *serv = READ_ONCE(xprt->server);
	if (skp_unlikely(!serv))
		return;
	spin_lock(&serv->lock);
	spin_unlock(&serv->lock);
}

static void xprt_dict_free(struct xprt* xprt)
{
	struct xprt *lstn;
	struct xprt_tcpclnt *clnt;
	struct server *serv = READ_ONCE(xprt->server);

#ifdef XPRT_DEBUG
	WARN_ON(xprt_event_delete(xprt) > 0);
#endif

	/*关闭描述符，在此之前一定要删除事件*/
	if (skp_likely(xprt_fd(xprt) > -1)) {
		bool wuwk = false;
		if ((xprt->flags & XPRT_OPT_TCPLARGELINGER) && xprt_is_tcpclnt(xprt)) {
			set_fd_block(xprt_fd(xprt));
			sockopt_set_linger(xprt_fd(xprt), true, 4096);
			wuwk = true;
			XPRT_WARN_ON(current_ev_worker());
			wq_worker_sleeping();
		}
		close(xprt_fd(xprt));
		if (wuwk)
			wq_worker_waking_up();
	}

	/*没有按照正确的流程 销毁 xprt*/
#ifdef XPRT_DEBUG
	WARN_ON(detach_xprt(xprt));
#endif

	/*释放对创建xprt的引用*/
	if (xprt_type(xprt) != XPRT_TCPTEMP)
		goto out;

	/*@see xprt_move()*/
	clnt = xprt_to_tcpclnt(xprt);
	if (!clnt->lstn_xprt)
		goto out;

	lstn = xchg_ptr(&clnt->lstn_xprt, NULL);
	XPRT_BUG_ON(!lstn);

	if (xprt_to_tcpserv(lstn)->cool_down) {
		/*必须加锁，与 destroy_xprt() 有竞争*/
		spin_lock(&serv->lock);
		/*enable read event*/
		if (skp_likely(!xprt_flags_closed(lstn->flags) &&
				!xprt_flags_detached(lstn->flags) &&
				xprt_to_tcpserv(lstn)->cool_down))
		{
			uint8_t mask = xprt_ev(lstn)->mask;
			/*如果失败也不关心，说明用户管理这个xprt的读写*/
			xprt_to_tcpserv(lstn)->cool_down = false;
			int rc = uev_stream_add(xprt_ev(lstn), mask|EVENT_READ);
			if (skp_unlikely(rc<0))
				LOG_XPRT_CHANGED_EVENT(lstn, EVENT_READ, ADD);
		}
		spin_unlock(&serv->lock);
	}

	xprt_put(lstn);

out:
	XPRT_WARN_ON(uref_read(&xprt->refs));
	XPRT_WARN_ON(xprt->flags & XPRT_ATTACHED);
	XPRT_WARN_ON(xprt->flags & XPRT_SHUT_RDWR);
	XPRT_WARN_ON(!(xprt->flags & XPRT_CLOSED));
	XPRT_WARN_ON(xprt->flags & XPRT_CONNECTING);

	/*释放内存*/
	xprt->xprt_ops->destructor(xprt);

	/*xprt 的内存释放时，可能还使用 serv 所以不能提前解除引用*/
	if (skp_likely(serv))
		server_put(serv);
}

static void xprt_rcu_free(void *ptr)
{
	struct xprt *xprt = ptr;
	if (WARN_ON(!xprt))
		return;
	xprt_dict_free(xprt);
}

/*仅关闭状态*/
static bool shutdown_xprt_stats(struct xprt *xprt, int how)
{
	bool rc;
	uint8_t ev = 0;
	unsigned long nflags, flags, mask = 0;

	/*计算需要关闭的事件和置位的状态*/
	/*0->1, 1->2, 2->3*/
	how++; /*@see inet_shutdown()*/
	if (how & (SHUT_RD + 1)) {
		mask |= XPRT_SHUTRD;
		ev |= EVENT_READ;
	}
	if (how & (SHUT_WR + 1)) {
		mask |= XPRT_SHUTWR;
		ev |= EVENT_WRITE;
	}

	do {
		flags = READ_ONCE(xprt->flags);
		/*已经关闭或已经设置了*/
		if (skp_unlikely((flags & XPRT_CLOSED)) ||
				((flags & mask) == mask) ||
				((mask != XPRT_SHUT_RDWR) && (mask & flags)))
			break;
		/* 关闭掉正在连接标志 */
		nflags = flags | mask;
		if (mask == XPRT_SHUT_RDWR)
			nflags &= ~ XPRT_CONNECTING;
		rc = cmpxchg(&xprt->flags, flags, nflags);
		if (skp_unlikely(!rc)) {
			LOG_XPRT_CHANGED_BIT(xprt, mask);
			continue;
		}

		/*
		 * 如果是在事件线程内调用，则还需要关闭相应的事件
		 * 因为内部回调函数可以处理相应的收尾工作
		 * 不需要保留事件来传递关闭
		 */
		if (current_ev_stream() == xprt_ev(xprt)) {
			int rc = uev_stream_disable(&xprt->event, ev);
			if (skp_unlikely(rc<0))
				LOG_XPRT_CHANGED_EVENT(xprt, ev, disable);
		}

		xprt_ops_call(xprt, on_shutdown, how - 1);

		return true;
	} while (1);

	return false;
}

static __always_inline void eat_xprt_open(struct xprt *xprt, uint16_t mask)
{
	bool rc;
	unsigned long flags;
	do {
		flags = READ_ONCE(xprt->flags);
		/*第一次变为可读/可写 ： 没有设置 opened ，且还没有被关闭*/
		if (skp_likely(flags & XPRT_OPENED))
			return;
		if (skp_unlikely(xprt_flags_closed(flags)))
			return;

		rc = cmpxchg(&xprt->flags, flags, flags | XPRT_OPENED);
		if (skp_likely(rc)) {
			/*invoke opened*/
			xprt_ops_call(xprt, on_changed, XPRT_OPENED | mask);
			return;
		}
		LOG_XPRT_CHANGED_BIT(xprt, XPRT_OPENED);
	} while (1);
}

/*返回非0值，关闭成功，需要销毁*/
static __always_inline int eat_xprt_close(struct xprt *xprt, uint16_t mask)
{
	int rc;
	unsigned long rflags, flags;
	do {
		flags = READ_ONCE(xprt->flags);
		/*已关闭*/
		if (skp_unlikely(flags & XPRT_CLOSED))
			return -ECONNABORTED;
		/*半关闭*/
		if (skp_likely((flags & XPRT_SHUT_RDWR) != XPRT_SHUT_RDWR))
			break;

		/*完全关闭*/
		rflags = cmpxchg_val(&xprt->flags, flags,
			(flags & ~(XPRT_SHUT_RDWR)) | XPRT_CLOSED);

		if (skp_unlikely(rflags != flags)) {
			LOG_XPRT_CHANGED_BIT(xprt, XPRT_CLOSED);
			continue;
		}

		/*必须触发过打开过才能触发关闭*/
		if (skp_likely(rflags & (XPRT_OPENED | XPRT_CONNREFUSED))) {
			/*invoke closed*/
			xprt_ops_call(xprt, on_changed, (rflags & XPRT_OPENED?
					XPRT_CLOSED:XPRT_CONNREFUSED) | mask);
			/*异步/同步删除*/
			rc = xprt_event_delete(xprt);
			if (skp_unlikely(rc<0))
				LOG_XPRT_CHANGED_EVENT(xprt, mask, delete);
		}
		return -ECONNABORTED;
	} while (1);

	return 0;
}

static __always_inline void eat_xprt_rdready(struct xprt *xprt, uint16_t mask)
{
	/*除了写事件，其他任何事件都需要通知读回调，因为缓存区内可能还有数据待处理*/
	if (!(mask & (EVENT_READ | EVENT_EOF | EVENT_ERROR)) || /*读端被关闭了*/
			skp_unlikely(xprt_status(xprt) & (XPRT_CLOSED|XPRT_SHUTRD)))
		return;

	/*invoke recv*/
	xprt_ops_call(xprt, on_recv, XPRT_RDREADY | mask);
}

static __always_inline void eat_xprt_wrready(struct xprt *xprt, uint16_t mask)
{
	/*尽可写才通知写回调*/
	if (!(mask & (EVENT_WRITE | EVENT_ERROR)) || /*写端被关闭了*/
			skp_unlikely(xprt_status(xprt) & (XPRT_CLOSED|XPRT_SHUTWR)))
		return;
	/*invoke send*/
	xprt_ops_call(xprt, on_send, XPRT_WRREADY | mask);

	/*shutdown read&write when socket got EVENT_ERROR*/
	if (skp_unlikely(mask & EVENT_ERROR))
		shutdown_xprt(xprt, SHUT_RDWR);
}

/*发起内部关闭*/
static __always_inline int intl_xprt_shutdown(struct xprt *xprt, uint16_t mask)
{
	set_bit(XPRT_CONNREFUSED_BIT, &xprt->flags);
	shutdown_xprt(xprt, SHUT_RDWR);
	return eat_xprt_close(xprt, EVENT_ERROR);
}

/*
 * @return 0 success, -EAGAIN in connecting , other error
 */
static int __eat_xprt_connecting(struct xprt *xprt, uint16_t mask)
{
	int rc = 0;
	struct sock_address sockaddr;
	struct xprt_tcpclnt *tcpclnt = xprt_to_tcpclnt(xprt);

	if (skp_unlikely(xprt_flags_closed(READ_ONCE(xprt->flags)))) {
		log_warn("nonblock connecting has been shutdown : %p/%lu",
			 xprt, xprt_status(xprt));
		return -ECONNABORTED;
	}

	XPRT_WARN_ON(!(xprt_opt(xprt) & XPRT_OPT_NONBLOCK));

	if (skp_unlikely(mask & EVENT_ERROR)) {
		rc = errno = sockopt_get_sockerr(xprt_fd(xprt));
		goto fail;
	}

	inet_address2sock(&tcpclnt->remote, &sockaddr);
	/*try to do connecting again*/
	rc = connect(xprt_fd(xprt), &sockaddr.sock_addr, sockaddr.length);
	/*还处于连接状态？ EALREADY*/
	if (skp_likely(rc)) {
		rc = errno;
		if (skp_unlikely(rc==EALREADY))
			return -EAGAIN;
		if (skp_unlikely(rc!=EISCONN))
			goto fail;
	}
#ifdef XPRT_DEBUG
	{
		struct sock_address saddr;
		char paddr[INET_ADDRESS_STRLEN];
		size_t l = inet_address2sock(&tcpclnt->local, &saddr);
		if (skp_likely(l) && skp_likely(!sockaddr_ntop(&saddr, paddr, sizeof(paddr)))) {
			log_info("active socket established : sfd [%d] -- local [%s:%u]",
				xprt_fd(xprt), paddr, inet_address_port(&tcpclnt->local));
		}

		l = inet_address2sock(&tcpclnt->remote, &saddr);
		if (skp_likely(l) && skp_likely(!sockaddr_ntop(&saddr, paddr, sizeof(paddr)))) {
			log_info("active socket established : sfd [%d] -- remote [%s:%u]",
				xprt_fd(xprt), paddr, inet_address_port(&tcpclnt->remote));
		}
	}
#endif

	/*非阻塞连接成功，调用可写回调*/
	return 0;
fail:
	log_warn("nonblocking connection failed : sfd [%d] : %s", xprt_fd(xprt),
		strerror_local());
	return intl_xprt_shutdown(xprt, EVENT_ERROR);
}

static __always_inline int eat_xprt_connecting(struct xprt *xprt, uint16_t mask)
{
	/*使用非 lock 指令预判断 减少cache sync*/
	if (!test_bit(XPRT_CONNECTING_BIT, &xprt->flags))
		return 0;

	/*查明阻塞连接或已连接*/
	if (!test_and_clear_bit(XPRT_CONNECTING_BIT, &xprt->flags))
		return 0;

	if (skp_unlikely(xprt_type(xprt) != XPRT_TCPCLNT)) {
		log_warn("nonblock connection is not suspported : %p/%lu",
			 xprt, xprt_type(xprt));
		return 0;
	}

	return __eat_xprt_connecting(xprt, mask);
}

/*握手*/
static __always_inline int __do_xprt_handshake(struct xprt *xprt, uint16_t mask)
{
	if (skp_unlikely(xprt_flags_closed(READ_ONCE(xprt->flags)))) {
		log_warn("connection has been shutdown : %p/%lu",
			 xprt, xprt_status(xprt));
		return -ECONNABORTED;
	}
	xprt_ops_call_ret(xprt, do_handshake, mask);
}

static __always_inline int do_xprt_handshake(struct xprt *xprt, uint16_t mask)
{
	if (test_bit(XPRT_HANDSHAKED_BIT, &xprt->flags))
		return 0;
	int rc = __do_xprt_handshake(xprt, mask);
	if (skp_likely(!rc)) {
		set_bit(XPRT_HANDSHAKED_BIT, &xprt->flags);
		return 0;
	} if (skp_likely(rc==-EAGAIN))
		return rc;

	/*
	 * 握手失败与连接都属于内部流程，如果失败则删除事件
	 * @see eat_xprt_close() 中的 on_change() 回调
	 */
	log_warn("xprt handshake failed : sfd [%d] : %s", xprt_fd(xprt),
		__strerror_local(-rc));
	return intl_xprt_shutdown(xprt, EVENT_ERROR);
}

static void process_xprt_event(struct uev_stream *ptr, uint16_t mask)
{
	int rc = 0;
	struct xprt *xprt = container_of(ptr, struct xprt, event);

#ifdef XPRT_DEBUG
	xprt = xprt_get(xprt);
	if (WARN_ON(!xprt))
		return;
#endif
	/*
	 * eat_xprt_connecting()/do_xprt_handshake() 如果没有成功都发起内部关闭
	 * 如果成功关闭则返回0，后续的流程都不运行，如果与 shutdown_xprt()/destroy_xprt()
	 * 发送竞争则会关闭失败，返回非 -EAGAIN 负值
	 * 即如果外部已经关闭套接字，则可以结束内部流程的运转，转 shut 标签处剥离 xprt
	 */
	/*消费各类事件*/
	rc = eat_xprt_connecting(xprt, mask);
	if (WARN_ON(rc == -EAGAIN))
		goto out;
	if (skp_unlikely(rc))
		goto shut;

	/*握手 @see xprt_ssl_handshake()*/
	rc = do_xprt_handshake(xprt, mask);
	if (rc == -EAGAIN)
		goto out;
	if (skp_unlikely(rc))
		goto shut;

	eat_xprt_open(xprt, mask);
	/*读优先*/
	eat_xprt_rdready(xprt, mask);
	eat_xprt_wrready(xprt, mask);
	rc = eat_xprt_close(xprt, mask);
	if (skp_likely(!rc))
		goto out;
shut:
	/*TODO:支持重连?*/
	/*成功关闭*/
	if (detach_xprt(xprt)) {
		/*TODO:此路径是否需要延迟释放*/
#ifdef XPRT_DEBUG
		XPRT_BUG_ON(uref_read(&xprt->refs) < 2);
		XPRT_BUG_ON(__uref_put(&xprt->refs));
#else
		XPRT_BUG_ON(uref_read(&xprt->refs) < 1);
		xprt_put(xprt);
#endif
	}
out:
#ifdef XPRT_DEBUG
	xprt_put(xprt);
#endif
	return;
}

static void xprt_tcp_setopt(int sfd, void *user)
{
	unsigned long opt = *(unsigned long*)user;
	if (skp_likely(opt & XPRT_OPT_NONBLOCK))
		XPRT_BUG_ON(set_fd_nonblock(sfd));
	if (opt & XPRT_OPT_TCPKEEPALIVE)
		sockopt_enable_keepalive(sfd, 1, 30, 12);
	if (opt & XPRT_OPT_TCPNAGLEOFF)
		sockopt_enable_nodelay(sfd);
	if (opt & XPRT_OPT_TCPLINGEROFF)
		sockopt_set_linger(sfd, false, 0);
}

int create_xprt_tcpserv(struct xprt* xprt, struct server *serv,
		const struct service_address *addr, unsigned long opt,
		const struct xprt_operations *lstn_ops,
		const struct xprt_operations *clnt_ops)
{
	int rc, sfd = 0;
	struct xprt_tcpserv *lstn = xprt_to_tcpserv(xprt);
	socklen_t slen = sizeof(lstn->lstn_address);

	if ((opt & XPRT_TYPE_MASK) != XPRT_TCPSERV)
		return -EINVAL;

	sfd = tcp_listen(addr, xprt_tcp_setopt, &opt);
	if (skp_unlikely(sfd < 0))
		return sfd;

	/*初始化tcpserv的字段和特定状态*/
	lstn->cool_down = false;
	lstn->clnt_xprt_ops = clnt_ops;
	/*获取本地地址*/
	rc = getsockname(sfd, &lstn->lstn_address.sock_addr, &slen);
	if (skp_unlikely(rc)) {
		rc = -errno;
		goto fail;
	}

#ifdef XPRT_DEBUG
{
	struct sock_address saddr;
	char paddr[INET_ADDRESS_STRLEN];
	size_t l = inet_address2sock(&lstn->lstn_address, &saddr);
	if (skp_likely(l) && skp_likely(!sockaddr_ntop(&saddr, paddr, sizeof(paddr)))) {
		log_debug("create listener socket : %d -- [%s:%u]",
			sfd, paddr, inet_address_port(&lstn->lstn_address));
	}
}
#endif

	/*安装时就关闭写端，这样写事件永远不会触发*/
	rc = attach_xprt(&lstn->xprt, sfd, XPRT_SHUTWR | opt, serv, lstn_ops);
	if (skp_unlikely(rc))
		goto fail;

	return 0;
fail:
	close(sfd);
	return rc;
}

/*关闭服务*/
void shutdown_xprt_tcpserv(struct xprt* xprt, int how)
{
	/*tcp服务器只需要关闭写/读端？*/
	struct xprt_tcpserv *txprt = xprt_to_tcpserv(xprt);
	if (shutdown_xprt_stats(&txprt->xprt, SHUT_RDWR))
		shutdown(xprt_fd(&txprt->xprt), SHUT_RD);
}

static inline void xprt_tcpserv_cooldown(struct xprt *xlstn)
{
	struct xprt_tcpserv *lstn = xprt_to_tcpserv(xlstn);
	if (skp_likely(!lstn->cool_down)) {
		struct server *serv = READ_ONCE(xlstn->server);
		if (WARN_ON(!serv))
			return;;
		spin_lock(&serv->lock);
		if (skp_likely(!lstn->cool_down)) {
			lstn->cool_down = true;
			int rc = xprt_event_delete(xlstn);
			if (skp_unlikely(rc < 0))
				LOG_XPRT_CHANGED_EVENT(xlstn, EVENT_READ, DELETE);
		}
		spin_unlock(&serv->lock);
	}
}

struct xprt *xprt_tcpserv_accept(struct xprt *xlstn, unsigned long stats)
{
	socklen_t slen;
	int rc, cfd = 0;
	unsigned long flags;
	struct xprt *xclnt = NULL;
	struct xprt_tcpclnt *clnt = NULL;
	struct xprt_tcpserv *lstn = xprt_to_tcpserv(xlstn);
	const struct xprt_operations *clntopt = lstn->clnt_xprt_ops;

	/*accept client*/
	if (xprt_has_closed(xlstn)) {
		errno = ENOTCONN;
		/*shutdown()不会参于实际的动作，主要是为了打印*/
		return NULL;
	}

	if (xprt_opt_check(clntopt)) {
		errno = EINVAL;
		return NULL;
	}

	cfd = tcp_accept(xprt_fd(xlstn), NULL);
	if (skp_unlikely(cfd < 0)) {
		if (skp_unlikely(cfd == -EMFILE)) {
			errno = EAGAIN;
			xprt_tcpserv_cooldown(xlstn);
		}
		return NULL;
	}

	/*准备安装*/
	flags = xprt_opt(xlstn) | XPRT_TCPTEMP;
	/*分配内存并初始化用户的子类*/
	xclnt = clntopt->constructor(xlstn->server, flags, xlstn);
	if (skp_unlikely(!xclnt))
		goto alloc_fail;

	clnt = xprt_to_tcpclnt(xclnt);
	clnt->lstn_xprt = xprt_get(xlstn);

	/*初始化 xprt_tcpclnt 字段*/
	/*获取本地地址*/
	slen = sizeof(clnt->local);
	rc = getsockname(cfd, &clnt->local.sock_addr, &slen);
	if (skp_unlikely(rc))
		goto setup_fail;

	/*获取远程地址*/
	slen = sizeof(clnt->remote);
	rc = getpeername(cfd, &clnt->remote.sock_addr, &slen);
	if (skp_unlikely(rc))
		goto setup_fail;

#ifdef XPRT_DEBUG
{
	struct sock_address saddr;
	char paddr[INET_ADDRESS_STRLEN];
	char laddr[INET_ADDRESS_STRLEN];
	size_t l = inet_address2sock(&lstn->lstn_address, &saddr);
	if (skp_likely(l) && skp_likely(!sockaddr_ntop(&saddr, laddr, sizeof(laddr)))) {
		l = inet_address2sock(&clnt->local, &saddr);
		if (skp_likely(l) && skp_likely(!sockaddr_ntop(&saddr, paddr, sizeof(paddr)))) {
			log_debug("create passive socket by %s : sfd [%d] -- local [%s:%u]",
				laddr, cfd, paddr, inet_address_port(&clnt->local));
		}

		l = inet_address2sock(&clnt->remote, &saddr);
		if (skp_likely(l) && skp_likely(!sockaddr_ntop(&saddr, paddr, sizeof(paddr)))) {
			log_debug("create passive socket by %s : sfd [%d] -- remote [%s:%u]",
				laddr, cfd, paddr, inet_address_port(&clnt->remote));
		}
	}
}
#endif

	/*linux 没有继承标志？*/
	xprt_tcp_setopt(cfd, &flags);

	/*安装*/
	rc = attach_xprt(xclnt, cfd, flags, xlstn->server, clntopt);
	if (skp_unlikely(rc)) {
		xprt_tcpserv_cooldown(xlstn);
		goto setup_fail;
	}

	/*返回此对象，引用计数由上层管理*/
	return xclnt;

setup_fail:
	/*除此路径，其他路径不可能知晓xprtclnt的存在，可以直接销毁*/
	if (xchg_ptr(&clnt->lstn_xprt, NULL))
		xprt_put(xlstn);
	clntopt->destructor(xclnt);

alloc_fail:
	log_warn("construct or setup clnt failed : %s", strerror_local());
	close(cfd);
	/*TODO:安装失败都忽略错误*/
	errno = EAGAIN;
	return NULL;
}

/*为侦听套接字预实现的操作，仅需直接使用或包裹使用*/
void xprt_tcpserv_recv(struct xprt *lstn, unsigned long stats)
{
	int rc;
	uint32_t nr_accepts = 0;
	struct xprt *clnt = NULL;

	do {
		if (skp_unlikely(nr_accepts >= CONFIG_MAX_ACCEPTS)) {
			/*TODO : startup timer and try again when timer was timedout
			 * 防止独占事件处理线程
			 * 流控？短时间大量连接判定为攻击？
			 * 保存上次满连接的时间戳？
			 */
			log_warn("!!!TOO MANY TCPXPRT CLIENT HAS "
					 "BEEN ALREADY, COOL DOWN THIS LISTENER!!!");
			break;
		}
		clnt = xprt_tcpserv_accept(lstn, stats);
		if (skp_unlikely(!clnt)) {
			if (skp_unlikely(errno != EAGAIN)) {
				log_warn("listener xprt [%p] occured error : %s",
					lstn, strerror_local());
				shutdown_xprt(lstn, SHUT_RDWR);
			}
			break;
		}
		/*启动读，失败则立即销毁传输对象*/
		rc = uev_stream_add(xprt_ev(clnt), EVENT_READ);
		if (WARN_ON(rc)) {
			destroy_xprt(clnt);
			continue;
		}
		xprt_put(clnt);
		nr_accepts++;
	} while (1);

	return;
}

void xprt_tcpserv_send(struct xprt *xprt, unsigned long stats)
{
	log_warn("impossible! it nerver invoke this function");
}

void xprt_tcpserv_changed(struct xprt *xprt, unsigned long stats)
{
	log_debug("xprt tcpserv's status has been changed : %p -> %lx",
			  xprt, stats & XPRT_STATS_MASK);
	return;
}

/*TODO:内存池*/
struct xprt *xprt_tcpserv_constructor(struct server *server, unsigned long opt, void *user)
{
	struct xprt_tcpserv *tcpxprt = malloc(sizeof(*tcpxprt));
	if (skp_unlikely(!tcpxprt))
		return NULL;
	memset(tcpxprt, 0, sizeof(*tcpxprt));
	tcpxprt->xprt.user = user;
	return &tcpxprt->xprt;
}

void xprt_tcpserv_destructor(struct xprt *xprt)
{
	struct xprt_tcpserv * tcpxprt = xprt_to_tcpserv(xprt);
	if (skp_likely(xprt))
		free(tcpxprt);
}

const struct xprt_operations xprt_tcpserv_ops = {
	.constructor = xprt_tcpserv_constructor,
	.destructor = xprt_tcpserv_destructor,
	.do_handshake = NULL,
	.on_recv = xprt_tcpserv_recv,
	.on_send = xprt_tcpserv_send,
	.on_changed = xprt_tcpserv_changed,
	.on_shutdown = NULL,
};

int create_xprt_tcpclnt(struct xprt *xprt, struct server *serv,
	const struct service_address *addr, unsigned long opt,
	const struct xprt_operations *clnt_ops)
{
	socklen_t slen;
	int flags, rc, sfd = 0;
	struct xprt_tcpclnt *clnt = xprt_to_tcpclnt(xprt);

	if ((opt & XPRT_TYPE_MASK) != XPRT_TCPCLNT)
		return -EINVAL;

	sfd = tcp_connect(addr, xprt_tcp_setopt, &opt, &flags, &clnt->remote);
	if (skp_unlikely(sfd < 0))
		return sfd;

	/*非阻塞连接的处理，紧接connect之后，防止修改errno？*/
	if (skp_likely(opt & XPRT_OPT_NONBLOCK) && (flags == EINPROGRESS))
		XPRT_BUG_ON(__test_and_set_bit(XPRT_CONNECTING_BIT, &opt));

	/*初始化其他字段*/
	clnt->lstn_xprt = NULL;

	/*获取本地地址*/
	slen = sizeof(clnt->local);
	rc = getsockname(sfd, &clnt->local.sock_addr, &slen);
	if (skp_unlikely(rc)) {
		rc = -errno;
		goto fail;
	}

	/*获取远程地址*/
#ifdef XPRT_DEBUG
{
	struct sock_address saddr;
	char paddr[INET_ADDRESS_STRLEN];
	size_t l = inet_address2sock(&clnt->local, &saddr);
	if (skp_likely(l) && skp_likely(!sockaddr_ntop(&saddr, paddr, sizeof(paddr)))) {
		log_info("create active socket : sfd [%d] -- local [%s:%u]",
			sfd, paddr, inet_address_port(&clnt->local));
	}

	if (!test_bit(XPRT_CONNECTING_BIT, &opt)) {
		l = inet_address2sock(&clnt->remote, &saddr);
		if (skp_likely(l) && skp_likely(!sockaddr_ntop(&saddr, paddr, sizeof(paddr)))) {
			log_info("create active socket : sfd [%d] -- remote [%s:%u]",
				sfd, paddr, inet_address_port(&clnt->remote));
		}
	}
}
#endif

	/*安装*/
	rc = attach_xprt(xprt, sfd, opt, serv, clnt_ops);
	if (skp_unlikely(rc))
		goto fail;

	return 0;
fail:
	close(sfd);
	__clear_bit(XPRT_CONNECTING_BIT, &xprt->flags);
	return rc;
}

void shutdown_xprt_tcpclnt(struct xprt *xprt, int how)
{
	/*需要再次引发事件，所以不关闭事件*/
	switch(how) {
		case SHUT_RD:
		case SHUT_WR:
		case SHUT_RDWR:
			if (shutdown_xprt_stats(xprt, how))
				shutdown(xprt_fd(xprt), how);
			break;
		default:
			log_warn("unknow operation passed to shutdown : %d", how);
	}
}

/*TODO : 内存池*/
struct xprt *xprt_tcpclnt_constructor(struct server *server, unsigned long opt,
		void *user)
{
	struct xprt_tcpclnt *xprtclnt = malloc(sizeof(*xprtclnt));
	if (skp_unlikely(!xprtclnt))
		return NULL;
	memset(xprtclnt, 0, sizeof(*xprtclnt));
	xprtclnt->xprt.user = user;
	return &xprtclnt->xprt;
}

void xprt_tcpclnt_destructor(struct xprt *xprt)
{
	struct xprt_tcpclnt *xprtclnt = xprt_to_tcpclnt(xprt);
	if (skp_likely(xprt))
		free(xprtclnt);
}

struct xprt *create_xprt(struct server *serv, const struct service_address *addr,
	unsigned long opt, const struct xprt_operations *xprt_ops, void *user, ...)
{
	int rc;
	uint8_t ev = 0;
	/*获取类型*/
	struct xprt *xprt = NULL;
	unsigned long type = opt & XPRT_TYPE_MASK;

	if (opt & XPRT_RDREADY)
		ev |= EVENT_READ;
	if (opt & XPRT_WRREADY)
		ev |= EVENT_WRITE;

	/*关闭事件状态*/
	opt &= ~(XPRT_RDREADY | XPRT_WRREADY);

	rc = create_xprt_check(serv, addr, opt, xprt_ops);
	if (skp_unlikely(rc))
		goto fail;

	rc = xprt_opt_check(xprt_ops);
	if (skp_unlikely(rc))
		goto fail;

	if (__server_has_stopped(serv)) {
		rc = -EINTR;
		goto fail;
	}

	/* 不一定非得是分配内存
	 * 也有可能从 user 参数中直接回去外部定义的（静态）内存
	 */
	xprt = xprt_ops->constructor(serv, opt, user);
	if (skp_unlikely(!xprt)) {
		rc = errno ? -errno : -ENOMEM;
		goto fail;
	}

	rc = -EINVAL;
	switch (type) {
		case XPRT_TCPSERV:
		{
			va_list ap;
			struct xprt_operations *clnt_ops;

			/*解析变参*/
			va_start(ap, user);
			clnt_ops = va_arg(ap, struct xprt_operations*);
			va_end(ap);

			if (WARN_ON(!clnt_ops))
				goto descon;

			rc = create_xprt_tcpserv(xprt, serv, addr, opt, xprt_ops, clnt_ops);
			if (skp_unlikely(rc))
				goto descon;
			/*侦听套接字不可能被写*/
			ev &= ~EVENT_WRITE;
			break;
		}
		case XPRT_TCPCLNT:
			rc = create_xprt_tcpclnt(xprt, serv, addr, opt, xprt_ops);
			if (skp_unlikely(rc))
				goto descon;
			break;
		case XPRT_TCPTEMP:
			log_warn("not allow create temp xprt");
			goto descon;
			/*TODO:其他类型，完全由构造函数初始化？*/
		default:
			log_warn("unknow type of xprt %p : %lu", xprt, type);
			goto descon;
	}

	XPRT_WARN_ON(ev && !(opt & XPRT_OPT_NONBLOCK));
	if (ev && skp_unlikely(rc = uev_stream_add(xprt_ev(xprt), ev))) {
		/*加入了容器，其他路径能看见此对象，所以必须安全销毁*/
		destroy_xprt(xprt);
		goto fail;
	}

	return xprt;

descon:
	/* 不一定是释放内存，也可以是通知持有静态内存的路径
	 * 以后框架不再被访问此传输对象*/
	xprt_ops->destructor(xprt);
fail:
	errno = -rc;
	log_debug("create [%lu] xprt fail : %s", type, strerror_local());
	return NULL;
}

void destroy_xprt(struct xprt *xprt)
{
	if (skp_unlikely(!xprt))
		return;

	maybe_sleep();
	/*
	 * 除非将下列所有操作加锁作为原子，否则可能与 xprt.event
	 * 事件对象的开启和禁用操作同时出发
	 */
	if (detach_xprt(xprt)) {
		XPRT_BUG_ON(uref_read(&xprt->refs) < 2);
		/*detach 成功，释放容器持有的引用*/
		XPRT_BUG_ON(__uref_put(&xprt->refs));
		/*直接强制关闭描述符？*/
	} else {
		/*detach 失败可能有其他路径操作，添加一个 栅栏，保证
		 *@see xprt_move() 的完成，然后才能销毁
		 */
		barrier_xprt(xprt);
	}

	/*同步停止事件，保证不会被事件线程引用*/
	uev_stream_delete_sync(xprt_ev(xprt));

	/*尝试强制关闭xprt*/
	/*可能已经被关闭了*/
	if (test_and_set_bit(XPRT_CLOSED_BIT, &xprt->flags))
		goto out;

	/*使用正确的情况下不会走以下路径*/
	do {
		/*强制停止*/
		bool rc;
		unsigned long flags = READ_ONCE(xprt->flags);
		/*因为已经设置了 closed，所以不需要有 半关闭 标志位*/
		rc = cmpxchg(&xprt->flags, flags, flags & (~XPRT_SHUT_RDWR));
		/*不应该有任何回调发生或状态改变*/
		if (skp_likely(rc))
			break;
		XPRT_WARN_ON(!uev_stream_delete_sync(xprt_ev(xprt)));
	} while (1);

	if (__test_and_clear_bit(XPRT_CONNECTING_BIT, &xprt->flags)) {
		/*查看是否还处于连接状态，如果是，则不调用关闭事件回调*/
		XPRT_WARN_ON(!(xprt->flags & XPRT_OPT_NONBLOCK));
	} else if (skp_likely(xprt_status(xprt) & XPRT_OPENED)) {
		if (xprt_status(xprt) & XPRT_HANDSHAKED)
			xprt_ops_call(xprt, on_shutdown, XPRT_CLOSED|EVENT_EOF);
		/*没有触发打开，则不会触发关闭*/
		xprt_ops_call(xprt, on_changed, XPRT_CLOSED|EVENT_EOF);
	}

out:
	/*释放该路径对 xprt 的引用*/
	xprt_put(xprt);
}

/*
 * 如果是在事件线程调用，则主动关闭对应的注册事件
 */
void shutdown_xprt(struct xprt *xprt, int how)
{
	unsigned long type;
	if (skp_unlikely(!xprt))
		return;
	type = xprt_type(xprt);
	switch (type) {
		case XPRT_TCPSERV:
			shutdown_xprt_tcpserv(xprt, how);
			break;
		case XPRT_TCPCLNT:
		case XPRT_TCPTEMP:
			shutdown_xprt_tcpclnt(xprt, how);
			break;
		default:
			log_warn("unknow type of xprt %p : %lu", xprt, type);
	}
	return;
}

void __xprt_put(struct uref *ptr)
{
	struct xprt *xprt = container_of(ptr, struct xprt, refs);
	if (skp_likely(!in_atomic())) {
		xprt_dict_free(xprt);
		return;
	}
	rcu_free(xprt, xprt_rcu_free);
	return;
}

void xprt_tcpclnt_init(struct xprt_tcpclnt *clnt, unsigned long flags,
		const struct xprt_operations * ops)
{
	struct xprt *xprt = &clnt->xprt;
	unsigned long type = flags & XPRT_TYPE_MASK;

	/*必须有销毁函数，否则 xprt_put() 将崩溃*/
	XPRT_BUG_ON(!ops);
	XPRT_BUG_ON(!ops->destructor);

	memset(clnt, 0, sizeof(*clnt));

	if (WARN_ON(type != XPRT_TCPCLNT && type != XPRT_TCPTEMP))
		type = XPRT_TCPCLNT;

	xprt->user = NULL;
	xprt->server = NULL;
	xprt->xprt_ops = ops;
	uref_init(&xprt->refs);
	INIT_LIST_HEAD(&xprt->node);
	/*初始化时为关闭的*/
	xprt->flags = type | XPRT_CLOSED;
	uev_stream_init(xprt_ev(xprt), -1, process_xprt_event);
}

static bool xprt_move(struct xprt *alias, struct xprt *src)
{
	int cpu = xprt_getcpu(src);
	unsigned long flags;
	struct server *serv = READ_ONCE(src->server);
	if (skp_unlikely(!serv))
		return false;

	XPRT_WARN_ON(cpu == EVENT_IDX_MAX);
	/*尽量在事件回调中调用，否则可能出现未知的错误*/
	if (current_ev_stream() == xprt_ev(src)) {
		xprt_event_delete(src);
	} else {
		log_warn("please invoke this function in event callback of xprt ...");
		xprt_event_delete_sync(src);
	}

	/*初始化*/

	/*
	 * 如果不处于附着状态，则表明已经处于销毁流程了
	 * 加锁来抢占 attached 位，这样可以使它优先级低于 destory_xprt()
	 * 更好的观察到 销毁流程
	 */
	spin_lock(&serv->lock);
	if (!test_and_clear_bit(XPRT_ATTACHED_BIT, &src->flags)) {
		spin_unlock(&serv->lock);
		return false;
	}

	alias->user = src->user;
	alias->flags = src->flags;
	alias->server = src->server;
	/*如果已经设置了操作回调，则不覆盖，否则使用相同的回调集合*/
	if (!alias->xprt_ops)
		alias->xprt_ops = src->xprt_ops;

	uref_init(&alias->refs);
	INIT_LIST_HEAD(&alias->node);
	uev_stream_init(xprt_ev(alias), xprt_fd(src), process_xprt_event);
	uev_stream_setcpu(xprt_ev(alias), cpu);

	/*剥离旧的*/
	XPRT_BUG_ON(list_empty(&src->node));
	XPRT_WARN_ON(src->server != serv);
	list_del_init(&src->node);

	/*原子的设置为关闭状态，并再次检查是否有其他路径同时操作
	 *@see destroy_xprt()
	 */
	flags = READ_ONCE(src->flags);
	XPRT_BUG_ON(!cmpxchg(&src->flags, flags, XPRT_CLOSED|xprt_type(src)));

	/*移除原 xprt 对描述符和 管理器的引用*/
	/*回调需要保留，因为需要释放内存*/
	src->user = NULL;
	src->server = NULL;
	uev_stream_init(xprt_ev(src), -1, NULL);

	/*嫁接新的*/
	xprt_get(alias);
	list_add_tail(&alias->node, &serv->xprt_list);
	XPRT_BUG_ON(__test_and_set_bit(XPRT_ATTACHED_BIT, &alias->flags));
	/* detach 成功，释放容器持有的引用
	 * 如果引用计数为0，不能马上释放，如果当前在事件回调中，则还会解引用
	 * @see process_xprt_event()
	 * 在锁内可以强制一个RCU异步释放
	 */
	xprt_put(src);

	spin_unlock(&serv->lock);

	return true;
}

bool xprt_tcpclnt_move(struct xprt_tcpclnt *alias, struct xprt_tcpclnt *src)
{
	if (!xprt_move(&alias->xprt, &src->xprt))
		return false;

	alias->local = src->local;
	alias->remote = src->remote;
	alias->lstn_xprt = xchg_ptr(&src->lstn_xprt, NULL);

	return true;
}

#ifdef ENABLE_SSL
#include <openssl/ssl.h>
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/conf.h>
#include <openssl/rand.h>

static bool ssl_inited = false;
static struct ssl_ctx_st *ssl_clntctx;
static struct ssl_ctx_st *ssl_servctx;
static __thread char ssl_ebuff[128];
static int process_tlsext_host_name(SSL *ssl, int *al, void *arg);

static const char *ssl_default_cer = "/tmp/ssl/certs/serverCert.cer";
static const char *ssl_default_key = "/tmp/ssl/certs/serverKey.pem";
static const char *ssl_gen_cerkey_sh = "\
mkdir -p /tmp/ssl/certs && \
cd /tmp/ssl/certs && \
openssl req -newkey rsa:2048 -nodes -keyout serverKey.pem \
-x509 -days 365 -out serverCert.cer \
-subj \"/C=CN/ST=SH/L=SH/O=skp.default.cert/OU=skp.default.key\" && \
cd -";

static void __ssl_init_env(void)
{
	big_lock();

	if (skp_unlikely(ssl_inited)) {
		big_unlock();
		return;
	}

	if (access(ssl_default_cer, R_OK) || access(ssl_default_key, R_OK)) {
		log_error("please usage like this :\n\n%s\n\n"
			"to generate default certificate file and private key file\n",
			ssl_gen_cerkey_sh);
		abort();
	}

	/*初始化环境*/
	OPENSSL_config(NULL);
	RAND_poll();
	SSL_library_init();
	SSL_load_error_strings();
	ERR_load_crypto_strings();
	OpenSSL_add_all_algorithms();

	/*创建默认的服务器和客户端上下文*/
	ssl_clntctx = SSL_CTX_new(SSLv23_client_method());
	ssl_servctx = SSL_CTX_new(SSLv23_server_method());
	if (skp_likely(ssl_clntctx)) {
		SSL_CTX_set_read_ahead(ssl_clntctx, 1);
		SSL_CTX_set_verify(ssl_servctx, SSL_VERIFY_NONE, NULL);
	}
	if (skp_likely(ssl_servctx)) {
		int rc;
		SSL_CTX_set_read_ahead(ssl_servctx, 1);
		rc = SSL_CTX_use_certificate_file(ssl_servctx, ssl_default_cer,
					SSL_FILETYPE_PEM);
		if (skp_unlikely(rc!=1)) {
			log_error("load certificate file [%s] failed : %s",
									ssl_default_cer, ssl_stack_error());
			abort();
		}
		rc = SSL_CTX_use_PrivateKey_file(ssl_servctx, ssl_default_key,
					SSL_FILETYPE_PEM);
		if (skp_unlikely(rc!=1)) {
			log_error("user private key file [%s] failed : %s",
									ssl_default_key, ssl_stack_error());
			abort();
		}
		rc = SSL_CTX_check_private_key(ssl_servctx);
		if (skp_unlikely(rc!=1)) {
			log_error("check private key failed : %s", ssl_stack_error());
			abort();
		}
		SSL_CTX_set_tlsext_servername_callback(ssl_servctx,
			process_tlsext_host_name);
	}

	WRITE_ONCE(ssl_inited, true);
	big_unlock();

	if (skp_unlikely(!ssl_clntctx)) {
		log_warn("initial SSL_CTX for client failed : %s", ssl_stack_error());
		abort();
	}
	if (skp_unlikely(!ssl_servctx)) {
		log_warn("initial SSL_CTX for server failed : %s", ssl_stack_error());
		abort();
	}
}

static inline void ssl_init_env(void)
{
	if (skp_likely(READ_ONCE(ssl_inited)))
		return;
	__ssl_init_env();
}

static inline const char *ssl_format_error(int rc0)
{
	snprintf(ssl_ebuff, sizeof(ssl_ebuff), "error [%s], func [%s], lib [%s]",
		ERR_reason_error_string(rc0), ERR_func_error_string(rc0),
		ERR_lib_error_string(rc0));
	return ssl_ebuff;
}

static int process_tlsext_host_name(SSL *ssl, int *al, void *arg)
{
	struct xprt_ssl *xptssl = SSL_get_app_data(ssl);
	BUG_ON(!xptssl);
	if (xptssl->servername_cb) {
		bool rc = xptssl->servername_cb(xptssl,
			SSL_get_servername(xptssl->ssl, TLSEXT_NAMETYPE_host_name));
		return skp_likely(rc)?SSL_TLSEXT_ERR_OK:SSL_TLSEXT_ERR_ALERT_FATAL;
	}
	return SSL_TLSEXT_ERR_OK;
}

/*
 * 检查返回值
 * @return 0 : 需要读，1 : 需要写，< 0 某种错误
 */
static __always_inline int ssl_check_ret(const struct ssl_st * ssl, int ret,
		const char *func)
{
	int rc = SSL_get_error(ssl, ret);

	if (skp_likely(rc == SSL_ERROR_WANT_READ)) {
		log_debug("[%s] need to read more data", func);
		/*poll on read*/
		return 0;
	} else if (skp_likely(rc == SSL_ERROR_WANT_WRITE)) {
		/*poll on write*/
		log_debug("[%s] need to write more data", func);
		return 1;
	} else if (skp_likely(rc == SSL_ERROR_ZERO_RETURN)) {
		log_warn("[%s] got end of file", func);
		return -ECONNRESET;
	} else if (skp_likely(rc == SSL_ERROR_SYSCALL)) {
		if (errno) {
			log_warn("[%s] got syscall error : %s", func, strerror_local());
			return -errno;
		}
	}

	unsigned long rc0 = ERR_peek_error();
	if (skp_likely(rc0 == 0)) {
		log_warn("[%s] got end of file", func);
		return -ECONNRESET;
	}

	log_warn("[%s] failed : %s", func, ssl_format_error(rc0));
	return -ECONNABORTED;
}

int ssl_set_default_certkey(const char *cert, const char *key)
{
	if (skp_unlikely(!cert || !key))
		return -EINVAL;

	if (access(cert, R_OK)||access(key, R_OK))
		return -ENOENT;

	big_lock();
	WARN_ON(ssl_servctx);
	ssl_default_cer = cert;
	ssl_default_key = key;
	big_unlock();

	return 0;
}

const char *ssl_stack_error(void)
{
	return ssl_format_error(ERR_get_error());
}

void ssl_stack_error_clear(void)
{
	while (ERR_peek_error())
		log_warn("SSL error stack : %s", ssl_stack_error());
	ERR_clear_error();
}

const char *xprt_ssl_error(const struct xprt_ssl *xprt, int ret)
{
	return ssl_format_error(SSL_get_error(xprt->ssl, ret));
}

int xprt_ssl_init(struct xprt_ssl *xprt, unsigned long opt, ...)
{
	struct ssl_ctx_st *ctx;
	unsigned long type = opt & XPRT_TYPE_MASK;

	BUG_ON(!xprt);

	ssl_init_env();

	if (!type) {
		va_list ap;
		/*解析变参*/
		va_start(ap, opt);
		ctx = va_arg(ap, struct ssl_ctx_st *);
		va_end(ap);
	} else if (type == XPRT_TCPCLNT) {
		ctx = ssl_clntctx;
	} else if (type == XPRT_TCPTEMP) {
		ctx = ssl_servctx;
	} else {
		return -EINVAL;
	}

	xprt->ctx = ctx;
	/*TODO:ssl 池*/

	xprt->ssl = SSL_new(ctx);
	if (skp_unlikely(!xprt->ssl))
		return -ENODEV;

	xprt->ev_mask = -1;
	xprt->tlsext_host_name = NULL;
	xprt->handshake_stat = XPRT_SSL_NONHANDSHAKE;
	/*初始化*/
	SSL_set_app_data(xprt->ssl, xprt);

	return 0;
}

void xprt_ssl_finit(struct xprt_ssl *xprt)
{
	if (skp_unlikely(xprt))
		return;
	if (xprt->ssl)
		SSL_free(xprt->ssl);
}

bool xprt_ssl_move(struct xprt_ssl *alias, struct xprt_ssl *src)
{
	if (!xprt_tcpclnt_move(&alias->tcp, &src->tcp))
		return false;

	alias->ctx = xchg_ptr(&src->ctx, NULL);
	alias->ssl = xchg_ptr(&src->ssl, NULL);
	alias->ev_mask = src->ev_mask;
	alias->handshake_stat = src->handshake_stat;

	return true;
}

void xprt_ssl_shutdown(struct xprt *xprt, int how)
{
	struct xprt_ssl *xptssl = xprt_to_ssl(xprt);

	/*握手未完成*/
	if (skp_unlikely(xptssl->handshake_stat != XPRT_SSL_HANDSHAKED))
		return;

	/*设置关闭方式并关闭*/
	how++;
	int ssl_how = 0;
	if (how & (SHUT_RD+1))
		ssl_how |= SSL_RECEIVED_SHUTDOWN;
	if (how & (SHUT_WR+1))
		ssl_how |= SSL_SENT_SHUTDOWN;

	SSL_set_shutdown(xptssl->ssl, ssl_how);
	int rc = SSL_shutdown(xptssl->ssl);
	/*tiny sleep*/
	//msleep_unintr(100);
	if (skp_likely(rc==1))
		return;

	/*出错*/
	if (skp_unlikely(rc < 0))
		goto out;

	/*尝试多次关闭*/
	bool nonblock = xprt->flags & XPRT_OPT_NONBLOCK;
	if (nonblock)
		set_fd_block(xprt_fd(xprt));

	int i = 4;
	do {
		rc = SSL_shutdown(xptssl->ssl);
	} while (rc==0 && --i>0);

	if (nonblock)
		set_fd_nonblock(xprt_fd(xprt));

	if (skp_unlikely(rc < 0)) {
out:
		log_error("SSL shutdown[%c/%c] failed : %s",
			(ssl_how & SSL_RECEIVED_SHUTDOWN)?'R':'_',
			(ssl_how & SSL_SENT_SHUTDOWN)?'W':'_', xprt_ssl_error(xptssl, rc));
	} else
		WARN_ON(rc==0);
}

int xprt_ssl_handshake(struct xprt *xprt, unsigned long event)
{
	int rc = 0;
	int type = xprt_type(xprt);
	bool is_clnt = !!(type == XPRT_TCPCLNT);
	struct xprt_ssl *xptssl = xprt_to_ssl(xprt);
	struct ssl_st *ssl = xptssl->ssl;

	XPRT_BUG_ON(!ssl);
	if (WARN_ON(type == XPRT_TCPSERV))
		return -EINVAL;

	if (WARN_ON(xptssl->handshake_stat == XPRT_SSL_HANDSHAKED))
		return 0;

	/*握手初始化*/
	if (xptssl->handshake_stat == XPRT_SSL_NONHANDSHAKE) {
#ifdef XPRT_DEBUG
		ssl_stack_error_clear();
#endif
		rc = SSL_set_fd(ssl, xprt_fd(xprt));
		if (skp_unlikely(rc!=1)) {
			log_error("initial SSL handshake failed : %s",
				xprt_ssl_error(xptssl, rc));
			return -ECONNABORTED;
		}
		if (is_clnt) {
			if (xptssl->tlsext_host_name)
				SSL_set_tlsext_host_name(ssl,xptssl->tlsext_host_name);
			SSL_set_connect_state(ssl);
		} else {
			SSL_set_accept_state(ssl);
		}

		/*首次由写触发，由于写事件只触发一次，所以需要保存，以便握手成功后恢复*/
		xptssl->ev_mask = -1;
		if (event & EVENT_WRITE)
			xptssl->ev_mask = EVENT_WRITE;
		xptssl->handshake_stat = XPRT_SSL_HANDSHAKING;
	}

#ifdef XPRT_DEBUG
	ssl_stack_error_clear();
#endif
	/*可能发生多次握手*/
	rc = SSL_do_handshake(ssl);
	if (skp_likely(rc == 1)) {
		BUG_ON(xptssl->handshake_stat!=XPRT_SSL_HANDSHAKING);
		xptssl->handshake_stat = XPRT_SSL_HANDSHAKED;
		/* initial handshake done, disable renegotiation (CVE-2009-3555) */
		if (ssl->s3)
			ssl->s3->flags |= SSL3_FLAGS_NO_RENEGOTIATE_CIPHERS;

		/*复原事件*/
		int ev_mask = xptssl->ev_mask;
		xptssl->ev_mask = -1;
		if (skp_likely(ev_mask==-1))
			return 0;
		if (ev_mask & EVENT_WRITE) {
			/*如果有写事件则使用者开启的应该启动，防止丢失*/
			rc = xprt_event_enable(xprt, EVENT_WRITE);
			if (skp_unlikely(rc<0))
				return skp_unlikely(rc==-EAGAIN)?-ECONNABORTED:rc;
		}
		if (skp_unlikely(ev_mask & EVENT_READ)) {
			/*如果有读事件则由握手开启的应该关闭*/
			rc = xprt_event_disable(xprt, EVENT_READ);
			if (skp_unlikely(rc<0))
				return skp_unlikely(rc==-EAGAIN)?-ECONNABORTED:rc;
		}
		return 0;
	}

	/*返回值检查*/
	rc = ssl_check_ret(ssl, rc, __FUNCTION__);
	if (skp_unlikely(rc<0))
		return rc;

	/*写事件是单次触发，一定需要开启*/
	int ev = EVENT_WRITE;
	if (rc==0) {
		/*want read*/
		int ev_mask = uev_stream_mask(xprt_ev(xprt));
		/*绝大多数应用都开启了读事件*/
		if (skp_likely(ev_mask & EVENT_READ))
			return -EAGAIN;
		ev = EVENT_READ;
		xptssl->ev_mask |= EVENT_READ;
	}
	rc = xprt_event_enable(xprt, ev);
	if (skp_unlikely(rc<0))
		return skp_unlikely(rc==-EAGAIN)?-ECONNABORTED:rc;
	return -EAGAIN;
}

ssize_t xprt_ssl_read(struct xprt *x, void *b, size_t s)
{
	XPRT_BUG_ON(!x);
	if (skp_unlikely(!b || s < 1))
		return 0;

	struct xprt_ssl *xptssl = xprt_to_ssl(x);

	ssl_stack_error_clear();
	int rc = SSL_read(xptssl->ssl, b, (int)s);
	if (skp_likely(rc > 0))
		return rc;
	rc = ssl_check_ret(xptssl->ssl, rc, __FUNCTION__);
	if (skp_unlikely(rc < 0))
		return rc==-ECONNRESET ? 0 : rc;
	if (skp_unlikely(rc)) {
		rc = xprt_event_enable(x, EVENT_WRITE);
		if (skp_unlikely(rc < 0))
			return skp_unlikely(rc==-EAGAIN)?-ECONNABORTED:rc;
	}
	return -EAGAIN;
}

ssize_t xprt_ssl_write(struct xprt *x, const void *b, size_t s)
{
	XPRT_BUG_ON(!x);
	if (skp_unlikely(!b || s < 1))
		return 0;

	struct xprt_ssl *xptssl = xprt_to_ssl(x);

	ssl_stack_error_clear();
	int rc = SSL_write(xptssl->ssl, b, (int)s);
	if (skp_likely(rc > 0))
		return rc;
	rc = ssl_check_ret(xptssl->ssl, rc, __FUNCTION__);
	if (skp_unlikely(rc < 0))
		return rc==-ECONNRESET ? 0 : rc;
	int ev = skp_likely(!rc) ? EVENT_READ: EVENT_WRITE;
	rc = xprt_event_enable(x, ev);
	if (skp_unlikely(rc < 0))
		return skp_unlikely(rc==-EAGAIN)?-ECONNABORTED:rc;
	return -EAGAIN;
}

#endif
