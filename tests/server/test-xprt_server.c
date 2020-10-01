//
//  xprt_server_test.c
//  test
//
//  Created by 周凯 on 2020/01/02.
//  Copyright © 2020 zhoukai. All rights reserved.
//

//#undef DEBUG

#include <stdio.h>
#include <skp/utils/pbuff.h>
#include <skp/process/event.h>
#include <skp/process/signal.h>
#include <skp/server/server.h>
#include <skp/server/xprt.h>

#include <skp/mm/slab.h>

//#define DEBUG

#ifndef BUFSIZE
# ifdef DEBUG
#  define BUFSIZE (8)
# else
#  define BUFSIZE ((4 << 10) - 8)
# endif
#endif

#define ECHO

#define to_my_server(serv) \
	({ BUG_ON(!(serv)); container_of((serv), struct my_server, server); })

#define to_my_listener(lstn) \
	({ BUG_ON(!(lstn)); container_of((lstn), struct my_listener, xprt.xprt); })

#define to_my_client(clnt) \
	({ BUG_ON(!(clnt)); container_of((clnt), struct my_client, xprt.xprt); })

#define pb_to_my_buff(pb) \
	({ BUG_ON(!(pb)); container_of((pb), struct my_buff, buff); })

#define node_to_my_buff(n) \
	({ BUG_ON(!(n)); container_of((n), struct my_buff, node); })

struct my_buff {
	struct pbuff buff;
	struct list_head node;
};

/*继承类*/
struct my_server {
	char name[32];
	struct server server;
};

/*继承类*/
struct my_listener {
	uint32_t nr_accepts;
	struct xprt_tcpserv xprt;
};

/*继承类*/
struct my_client {
	uint32_t nr_recvs;
	struct xprt_tcpclnt xprt;
	struct list_head send_queue;
};

/*继承类销毁函数*/
static void destroy_my_server(struct server * __serv)
{
	struct my_server *serv = to_my_server(__serv);
	log_info("server [%s] will be destroyed", serv->name);
}

/*继承类的构造函数*/
static struct xprt *construtor_listener(struct server *serv, unsigned long opt,
		void *user)
{
	struct my_listener *listener= malloc(sizeof(*listener));
	BUG_ON(!listener);

	listener->nr_accepts = 0;
	/*只能裸使用这一个字段*/
	listener->xprt.xprt.user = user;

	log_info("listener [%p] will be created", listener);

	return &listener->xprt.xprt;
}

static void destructor_listener(struct xprt *__xprt)
{
	struct my_listener *listener = to_my_listener(__xprt);

	log_info("listener [%p] will be destroy : %lx", listener, __xprt->flags);

	free(listener);
}

static struct xprt *construtor_client(struct server *serv, unsigned long opt,
		void *user)
{
	struct my_client* client = malloc(sizeof(*client));
	if (WARN_ON(!client))
		return NULL;

	/*能裸使用仅这一个字段*/
	client->nr_recvs = 0;
	client->xprt.xprt.user = user;
	INIT_LIST_HEAD(&client->send_queue);

	log_info("client [%p] will be created, current count of client : %d",
		&client->xprt.xprt, serv->nr_xprts);

	return &client->xprt.xprt;
}

static void destructor_client(struct xprt *xprt)
{
	struct my_buff *mpb, *n;
	struct my_client *cli = to_my_client(xprt);

	if (xprt->server) {
		log_info("client [%p] will be destroy, current count of client : %d",
			xprt, xprt->server->nr_xprts);
	}

	list_for_each_entry_safe(mpb, n, &cli->send_queue, node) {
		free_pb(&mpb->buff);
	}

	free(cli);
}

static struct pbuff *constructor_mybuff(void *user)
{
	struct my_client *cli = user;
	struct my_buff *mpb = malloc(sizeof(*mpb));
	if (skp_unlikely(!mpb))
		return NULL;
	mpb->buff.user = cli;
	list_add(&mpb->node, &cli->send_queue);
	return &mpb->buff;
}

static void destructor_mybuff(struct pbuff* pb)
{
	struct my_buff *mpb = pb_to_my_buff(pb);
	if (!list_empty(&mpb->node))
		list_del(&mpb->node);
	free(mpb);
}

static const struct pb_ops pb_opt = {
	.constructor = constructor_mybuff,
	.destructor = destructor_mybuff,
	.clone = NULL,
	.copy = NULL,
	.expand = NULL,
};

static struct my_buff *get_send_buff(struct my_client *cli, ssize_t *l)
{
	struct pbuff *pb;
	struct my_buff *mpb = list_first_entry_or_null(&cli->send_queue,
			struct my_buff, node);
	if (mpb && (*l = pb_tailroom(&mpb->buff)))
		return mpb;
	pb = __alloc_pb(BUFSIZE, &pb_opt, cli);
	if (skp_unlikely(!pb))
		return NULL;
	*l = BUFSIZE;
	return pb_to_my_buff(pb);
}

static void client_send(struct xprt *xprt, unsigned long stats)
{
	struct my_buff *mpb, *n;
	struct my_client *client = to_my_client(xprt);

	/*反向遍历开始发送*/
	list_for_each_entry_safe_reverse(mpb, n, &client->send_queue, node) {
#ifdef ECHO
		ssize_t rc, l;
		while ((l = pb_headlen(&mpb->buff))) {
			rc = xprt_write(xprt, pb_data(&mpb->buff), l);
			if (rc < 0) {
				if (skp_unlikely(rc != -EAGAIN))
					goto shut;
				/*开启写*/
				xprt_event_enable(xprt, EVENT_WRITE);
				return;
			}
			/*更新指针，移除已发送的数据*/
			pb_pulldata(&mpb->buff, rc);
		}
		/*发送完毕，如果没有尾部空间，则可以释放*/
		if (!pb_tailroom(&mpb->buff))
			free_pb(&mpb->buff);
#else
		free_pb(&mpb->buff);
#endif
	}

	/*读端已经关闭，且没有更多的数据，则关闭*/
	if (!xprt_has_shutrd(xprt))
		return;

shut:
	shutdown_xprt(xprt, SHUT_RDWR);
}

static void client_recv(struct xprt *xprt, unsigned long stats)
{
	ssize_t rc, l, nr = 16;
	struct my_client *cli = to_my_client(xprt);
	struct my_buff *mpb;

	do {
		/*获取缓存*/
		mpb = get_send_buff(cli, &l);
		/*内存不足，就发送*/
		if (skp_unlikely(!mpb))
			goto start_send;

		/*读取*/
		rc = xprt_read(xprt, pb_tail(&mpb->buff), l);
		if (!rc) {
			shutdown_xprt(xprt, SHUT_RD);
			break;
		}

		if (rc < 0) {
			if (skp_unlikely(rc != -EAGAIN))
				goto shut;
			/*没有更多的数据，可以开始发送*/
			goto start_send;
		}

		/*更新缓存*/
		pb_putdata(&mpb->buff, rc);
	} while (--nr > 0);

start_send:
	client_send(xprt, XPRT_WRREADY|EVENT_WRITE);
	return;
shut:
	shutdown_xprt(xprt, SHUT_RDWR);
}

static void client_changed(struct xprt *__xprt, unsigned long stats)
{
	struct my_client *client = to_my_client(__xprt);
	if (stats & XPRT_OPENED) {
		log_info("create one connection : %p", client);
	} else if (stats & XPRT_CLOSED) {
		log_info("destroy one connection : %p", client);
	} else {
		BUG();
	}
}

static const struct xprt_operations listen_ops = {
	.constructor = construtor_listener,
	.destructor = destructor_listener,
	.on_recv = xprt_tcpserv_recv,
	.on_send = xprt_tcpserv_send,
	.on_changed = xprt_tcpserv_changed,
};

static const struct xprt_operations client_ops = {
	.constructor = construtor_client,
	.destructor = destructor_client,
	.on_recv = client_recv,
	.on_send = client_send,
	.on_changed = client_changed,
};

static struct my_server *SRV = NULL;

static void server_init(void)
{
	struct server *srv;

	log_info("create server");

	signal_setup(SIGPIPE, signal_default);

	srv = ___alloc_server(sizeof(struct my_server),128,0);
	SRV = to_my_server(srv);
	/*initialize subclass's field : named struct*/
	srv->destructor = destroy_my_server;
	snprintf(SRV->name, sizeof(SRV->name), "%s", "test_server");

	struct xprt *listen;
	const struct service_address laddr = {
		.host = "0.0.0.0",
		.serv = "10000",
	};

	/*启动了读事件，可以接受并创建新连接*/
	listen = create_xprt(srv,&laddr,XPRT_TCPSERV|XPRT_OPT_NONBLOCK|XPRT_RDREADY,
				&listen_ops, NULL, &client_ops);

	/*可以在此将对象传递给其他对象、线程进行管理和使用*/
	xprt_put(listen);
}

static void signal_handle(struct uev_signal *ptr)
{
	log_info("interrupt by SIGINT, program will be shutdown...");
	server_pause(&SRV->server);
}

static void start_loop(void)
{
	struct uev_signal signal;

	log_info("start loop");
	uev_signal_init(&signal, SIGINT, signal_handle);
	BUG_ON(uev_signal_register(&signal));
	server_loop(&SRV->server);
	uev_signal_unregister_sync(&signal);
}

static void server_finit(void)
{
	log_info("destroy server");
	destroy_server(&SRV->server);
}

int main(int argc, const char *argv[])
{
	server_init();
	start_loop();
	server_finit();
	return 0;
}
