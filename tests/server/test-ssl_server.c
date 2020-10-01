#include <skp/server/xprt.h>
#include <skp/server/server.h>

static struct xprt * construtor_client(struct server *serv, unsigned long opt,
		void *user)
{
	struct xprt_ssl *ssl = malloc(sizeof(*ssl));
	int rc = xprt_ssl_init(ssl, opt);
	BUG_ON(rc);

	return &ssl->tcp.xprt;
}

static void destructor_client(struct xprt *xprt)
{
	log_warn("ssl has been destroy ...");
}

static void client_recv(struct xprt *xprt, unsigned long stats)
{
	log_info("readable");
	char buff[64];
	do {
		ssize_t rc = xprt_ssl_read(xprt, buff, sizeof(buff));
		if (rc < 1) {
			if (rc != -EAGAIN)
				shutdown_xprt(xprt, SHUT_RDWR);
			return;
		}
		ssize_t rc0 = xprt_ssl_write(xprt, buff, rc);
		if (rc0!=rc) {
			shutdown_xprt(xprt, SHUT_RDWR);
			return;
		}
	} while(1);
}

static void client_send(struct xprt *xprt, unsigned long stats)
{
	log_info("writeable");
}

static void client_changed(struct xprt *xprt, unsigned long stats)
{
	if (stats & XPRT_OPENED) {
		log_info("connect success ...");
	} else if (stats & XPRT_CONNREFUSED) {
		log_error("connect failure ...");
	}
}

static struct server *SRV = NULL;

static const struct xprt_operations client_ops = {
	.constructor = construtor_client,
	.destructor = destructor_client,
	.on_recv = client_recv,
	.on_send = client_send,
	.on_changed = client_changed,
	.do_handshake = xprt_ssl_handshake,
	.on_shutdown = xprt_ssl_shutdown,
};

int main(int argc, char **argv)
{
	SRV = ___alloc_server(sizeof(*SRV), 16, 0);
	BUG_ON(!SRV);
	const struct service_address host_addr = {
		.host = "127.0.0.1",
		.serv = "1443",
	};

	struct xprt * ssl = create_xprt(SRV, &host_addr,
			XPRT_TCPSERV|XPRT_OPT_NONBLOCK|XPRT_RDREADY,
			&xprt_tcpserv_ops, (void*)&host_addr, &client_ops);
	BUG_ON(!ssl);

	xprt_put(ssl);

	server_loop(SRV);

	return 0;
}

