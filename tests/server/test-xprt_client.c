//
//  xprt_server_test.c
//  test
//
//  Created by 周凯 on 2020/01/02.
//  Copyright © 2020 zhoukai. All rights reserved.
//

#include <stdio.h>
#include <skp/utils/pbuff.h>
#include <skp/process/event.h>
#include <skp/process/signal.h>
#include <skp/server/server.h>
#include <skp/server/xprt.h>
#include <skp/mm/slab.h>

#define ECHO 1

#ifndef BUFSIZE
# define BUFSIZE (1 << 12)
#endif

#define to_my_server(serv) \
	({ BUG_ON(!(serv)); container_of((serv), struct my_server, server); })

#define to_my_client(clnt) \
	({ BUG_ON(!(clnt)); container_of((clnt), struct my_client, xprt.xprt); })

/*以下展示了如何进行简单伪面向对象编程*/

/*struct server 的继承类*/
struct my_server {
	struct server server;
	char name[32];
};

/*struct xprt_tcpclnt 继承类*/
struct my_client {
	ssize_t nr_send;
	struct uev_timer timer;
	struct xprt_tcpclnt xprt;
};

/*我们只是为了展示库的使用，数据是无效的
 *一些脏数据公共缓冲区
 */
static char BUFF[BUFSIZE];

/*继承类销毁函数*/
static void destroy_my_server(struct server * __serv)
{
	struct my_server *serv = to_my_server(__serv);
	log_info("server [%s] will be destroyed", serv->name);
}

static void client_timer(struct uev_timer *timer)
{
	struct my_client *cli = container_of(timer, struct my_client, timer);
	/*准备数据*/
	cli->nr_send = BUFSIZE;
	/* 可以将定时器在连接建立成功后，分配到相同的事件循环中，这样可以串行化写：
	 * 在定时器回调中写数据，如果因为缓存区不足，写出失败，才启动写事件
	 * 这样做可以减少大量的系统调用和上下文切换
	 */
	/*重新启动写*/
	xprt_event_enable(&cli->xprt.xprt, EVENT_WRITE);
}

static struct xprt * construtor_client(struct server *serv, unsigned long opt,
		void *user)
{
	struct my_client *cli = malloc(sizeof(*cli));
	BUG_ON(!cli);

	cli->nr_send = 0;
	uev_timer_init(&cli->timer, client_timer);
	log_info("client [%p] will be created", &cli->xprt.xprt);

	return &cli->xprt.xprt;
}

static void destructor_client(struct xprt *xprt)
{
	struct my_client *cli = to_my_client(xprt);

	log_info("client [%p] will be destroy", xprt);

	uev_timer_delete_sync(&cli->timer);
	free(cli);
}

static void client_recv(struct xprt *xprt, unsigned long stats)
{
	ssize_t rc;

	do {
		rc = xprt_read(xprt, BUFF, sizeof(BUFF));
		if (rc < 1) {
			if (!rc || rc != -EAGAIN)
				shutdown_xprt(xprt, SHUT_RDWR);
			break;
		}
	} while (1);

	return;
}

static void client_send(struct xprt *xprt, unsigned long stats)
{
	ssize_t rc;
	struct my_client *cli = to_my_client(xprt);

	/*尝试发送数据*/
	while (cli->nr_send) {
		rc = xprt_write(xprt, BUFF, cli->nr_send);
		if (skp_unlikely(rc < 0)) {
			if (rc == -EAGAIN) {
				xprt_event_enable(xprt, EVENT_WRITE);
			} else {
				shutdown_xprt(xprt, SHUT_RDWR);
			}
			break;
		}
		cli->nr_send -= rc;
	}

	/*发送完毕，启动定时器，准备下次的发送*/
	if (!cli->nr_send)
		uev_timer_add(&cli->timer, 10);

	if (prandom_chance(1.0f/500))
		log_info("xprt [%p] send [%ld] bytes", cli, BUFSIZE - cli->nr_send);

	return;
}

static void client_changed(struct xprt *xprt, unsigned long stats)
{
	struct my_client *client = to_my_client(xprt);
	if (stats & XPRT_OPENED) {
		/*dispatch到相同的CPU上*/
		int c = uev_stream_getcpu(xprt_ev(xprt));
		uev_timer_setcpu(&client->timer, c);
		/*连接成功，你可以同步删除定时器，然后重新将其分配到该回调的所在事件上下文中*/
		/*初始化发送量*/
		client->nr_send = BUFSIZE;
#ifdef ECHO
		xprt_event_enable(xprt, EVENT_READ);
#endif
		log_info("create one connection : %p", client);
	} else if (stats & XPRT_CLOSED) {
		log_info("destroy one connection : %p", client);
	} else if (stats & XPRT_CONNREFUSED) {
		log_error("connect has been refused : %p", client);
	} else {
		log_error("BUG : framework error ...");
		BUG();
	}
}

static const struct xprt_operations client_ops = {
	.constructor = construtor_client,
	.destructor = destructor_client,
	.on_recv = client_recv,
	.on_send = client_send,
	.on_changed = client_changed,
};

static struct my_server *SRV = NULL;

static void server_init(int argc, const char **argv)
{
	struct xprt *cli;
	struct server *srv;
	int nr_cli = argc > 2 ? atoi(argv[2]) : 1;
	const struct service_address laddr = {
		.host = argv[1],
		.serv = "10000",
	};

	log_info("create server");

	signal_setup(SIGPIPE, signal_default);

	/*server 只能简单的被继承，所以基类必须是第一个字段
	 *框架内的所有内置简单继承都是这样的：
	 *1. 内存必须框架提供
	 *2. 基类必须为第一个字段
	 *3. 至多继承一次
	 *不论 uthread_t 还是 struct server 他们的功能都很单一
	 *线程仅提供一个执行上下文和线程类型标识，通过封装 workqueue 我们根本不需要
	 *继承线程来工作，同样 struct server 仅仅是聚集所有创建的 xprt ，因为我们为
	 *了保证优雅的退出程序，必须在这之前 destory_xprt()，如果你想分类管理 xprt，
	 *就应该通过继承 xprt 增加新的 节点 字段，然后添加到自己的容器中并保持引用
	 *计数
	 */
	srv = ___alloc_server(sizeof(struct my_server),nr_cli>1?nr_cli:8,0);
	SRV = to_my_server(srv);
	/*initialize subclass's field : named struct*/
	srv->destructor = destroy_my_server;
	snprintf(SRV->name, sizeof(SRV->name), "%s", "test_server");

	/*创建一些主动连接，必须开启写，才能监测到连接成功事件*/

	for (int i = 0; i < nr_cli; i++) {
#if 0
		/*异步连接*/
		cli = create_xprt(srv, &laddr,
			XPRT_TCPCLNT|XPRT_OPT_NONBLOCK|XPRT_WRREADY, &client_ops, NULL);
		/*可以在此将对象传递给其他对象、线程进行管理和使用*/
		if (skp_unlikely(!cli))
			break;
#else
		/*同步连接，连接成功后，加入事件循环前，必须设置为非阻塞*/
		cli = create_xprt(srv, &laddr, XPRT_TCPCLNT, &client_ops, NULL);
		if (skp_unlikely(!cli))
			break;
		xprt_set_nonblock(cli);
		xprt_event_add(cli, EVENT_WRITE);
#endif
		/* 由于框架内部保存了一份引用计数，如果我们不需要保存一定要释放
		 * create_xprt() 函数返回给本路径的引用计数 */
		xprt_put(cli);
		msleep_unintr(10);
	}
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
	/*必须同步删除信号事件*/
	uev_signal_unregister_sync(&signal);
}

static void server_finit(void)
{
	log_info("destroy server");
	destroy_server(&SRV->server);
}

int main(int argc, const char *argv[])
{
	if (skp_unlikely(argc != 2 && argc != 3)) {
		log_error("usage ./test-xprt_client <host addr> [#client]");
		return EXIT_FAILURE;
	}

	server_init(argc, argv);
	start_loop();
	server_finit();

	return EXIT_SUCCESS;
}
