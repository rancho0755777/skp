/*
 * 使用一个简单的 echo 网络服务器来测试
 */
#include <skp/process/event.h>
#include <skp/server/socket.h>
#include <skp/utils/spinlock.h>
#include <skp/process/signal.h>
#include <skp/process/completion.h>

#include <skp/mm/slab.h>

enum {
	SRV_SOCK,
	CLI_SOCK,
};

//#define ECHO

#ifdef TEST_EDGE
# define DEF_EVMASK EVENT_EDGE
#else
# define DEF_EVMASK 0
#endif

static DEFINE_SPINLOCK(obj_lock);
/*将所有创建的对象管理起来，方便在退出时优雅的销毁*/
static LIST__HEAD(obj_list);

static void srv_ready(struct uev_stream *event, uint16_t mask);
static void cli_ready(struct uev_stream *event, uint16_t mask);

struct sock_base {
	uint32_t type;
	struct uev_stream event;
	struct list_head node;
};

struct sock_cli {
	struct sock_base sock;
#ifdef ECHO
	char buff[32];
#else
	char buff[4096];
#endif
};

struct sock_srv {
	struct sock_base sock;
	int nr_cli;
};

struct sigint {
	struct uev_signal base;
	completion_t done;
};

static void server_create(void)
{
	struct service_address addr = {
		"127.0.0.1", "10000"
	};

	signal_block_all(NULL);

	int sfd = tcp_listen(&addr, NULL, NULL);
	BUG_ON(sfd < 0);
	set_fd_nonblock(sfd);

	struct sock_srv *srv = malloc(sizeof(*srv));
	BUG_ON(!srv);

	srv->nr_cli = 0;
	srv->sock.type = CLI_SOCK;
	uev_stream_init(&srv->sock.event, sfd, srv_ready);
	list_add_tail(&srv->sock.node, &obj_list);

	int rc = uev_stream_add(&srv->sock.event, EVENT_READ);
	if (skp_unlikely(rc)) {
		log_error("register event failed : %s", __strerror_local(-rc));
		BUG();
	}
}

static void signal_handle(struct uev_signal *ptr)
{
	struct sigint *sigint = container_of(ptr, struct sigint, base);
	log_info("interrupt by SIGINT, program will be shutdown...");
	complete(&sigint->done);
}

static void server_loop(void)
{
	struct sigint signal;
	init_completion(&signal.done);
	uev_signal_init(&signal.base, SIGINT, signal_handle);

	BUG_ON(uev_signal_register(&signal.base));
	wait_for_completion(&signal.done);
	uev_signal_unregister_sync(&signal.base);
}

static inline void __free_sock(struct sock_base *sock)
{
	if (sock->type == SRV_SOCK) {
		free(container_of(sock, struct sock_srv, sock));
	} else {
		free(container_of(sock, struct sock_cli, sock));
	}
}

static void free_sock(struct sock_base *sock)
{
	bool f = false;
	/*
	 * 如果不在链表里，说明正在被销毁中
	 * @see server_destory()
	 */
	spin_lock(&obj_lock);
	if (!list_empty(&sock->node)) {
		f = true;
		/*仍在链表中，说明与server_destory() 没有发生竞争*/
		list_del(&sock->node);
	}
	spin_unlock(&obj_lock);

	if (skp_likely(f))
		__free_sock(sock);
}

static void server_destory(void)
{
	struct sock_base *sock;

	spin_lock(&obj_lock);
	while (!list_empty(&obj_list)) {
		sock = list_first_entry(&obj_list, struct sock_base, node);
		list_del_init(&sock->node);
		spin_unlock(&obj_lock);

		/*同步删除，保证事件回调不在引用此对象*/
		if (uev_stream_delete_sync(&sock->event) > 0)
			uev_stream_closefd(&sock->event);

		__free_sock(sock);
		spin_lock(&obj_lock);
	}
	spin_unlock(&obj_lock);

	signal_unblock_all(NULL);
}

static inline void shut_sock(struct sock_base *sock)
{
	if (uev_stream_delete_async(&sock->event) > 0)
		uev_stream_closefd(&sock->event);
	free_sock(sock);
}

static void srv_ready(struct uev_stream *ev, uint16_t mask)
{
	/*从基类转换为继承类*/
	int rc, fd;
	struct sock_cli *cli;
	union inet_address addr;
#ifdef DEBUG
	struct sock_address saddr;
	char address[INET_ADDRESS_STRLEN];
#endif
	struct sock_srv *srv = container_of(ev, struct sock_srv, sock.event);

	do {
		fd = tcp_accept(uev_stream_fd(ev), &addr);
		if (skp_unlikely(fd < 0)) {
			if (skp_likely(fd == -EAGAIN))
				break;
			goto shut;
		}
		srv->nr_cli++;
		/*新的连接*/
#ifdef DEBUG
		inet_address2sock(&addr, &saddr);
		sockaddr_ntop(&saddr, address, sizeof(address));
		log_info("new connection : %s , %d", address, inet_address_port(&addr));
#endif

		cli = malloc(sizeof(*cli));
		if (WARN_ON(!cli)) {
			close(fd);
			break;
		}

		cli->sock.type = CLI_SOCK;
		uev_stream_init(&cli->sock.event, fd, cli_ready);

		/*必须先加入管理链表，才能启动事件*/
		spin_lock(&obj_lock);
		list_add_tail(&cli->sock.node, &obj_list);
		spin_unlock(&obj_lock);

		rc = uev_stream_add(&cli->sock.event, EVENT_READ);
		if (skp_unlikely(rc)) {
			log_error("register event failed : %s", __strerror_local(-rc));
			uev_stream_closefd(&cli->sock.event);
			free_sock(&cli->sock);
		}

	} while (1);

	return;
shut:
	shut_sock(&srv->sock);
}

static void cli_ready(struct uev_stream *ev, uint16_t mask)
{
	ssize_t l;
#ifdef ECHO
	ssize_t w;
	const char *ptr;
#endif
	/*从基类转换为继承类*/
	struct sock_cli *cli = container_of(ev, struct sock_cli, sock.event);
	/*这里只是展示怎么使用事件，所以只关心读，而假设可以一致性回射接收的数据*/
	do {
		l = read(uev_stream_fd(ev), cli->buff, sizeof(cli->buff));
		if (l == 0)
			goto shut;
		if (l < 0) {
			if (errno != EAGAIN)
				goto shut;
			break;
		}

#ifdef ECHO
		ptr = &cli->buff[0];
		while (l > 0) {
			w = write(uev_stream_fd(ev), ptr, l);
			if (skp_unlikely(w < 0)) {
				if (errno != EAGAIN)
					goto shut;
				sched_yield();
				continue;
			}
			l -= w;
			ptr += w;
			BUG_ON(l < 0);
		}
#endif
	} while (1);

	return;
shut:
	/*必须异步关闭事件之后才关闭描述符*/
	shut_sock(&cli->sock);
}

int main(int argc, char const *argv[])
{
	server_create();
	server_loop();
	server_destory();
	return 0;
}
