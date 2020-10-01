#include <skp/server/xprt.h>
#include <skp/server/server.h>
#include <openssl/ssl.h>

static const char https_buff[] = "\
GET /api/general/v3/time HTTP/1.1\n\
Host: www.okex.com\n\n";

static struct xprt * construtor_client(struct server *serv, unsigned long opt,
		void *user)
{
	struct xprt_ssl *ssl = malloc(sizeof(*ssl));
	int rc = xprt_ssl_init(ssl, opt);
	BUG_ON(rc);
	const struct service_address *host_addr = user;
	xprt_ssl_set_tlsext_servername(ssl, host_addr->host);

	return &ssl->tcp.xprt;
}

static void destructor_client(struct xprt *xprt)
{
	log_warn("ssl has been destroy ...");
}

static void client_recv(struct xprt *xprt, unsigned long stats)
{
	char buff[128];
	log_info("readable");
	//shutdown_xprt(xprt, SHUT_RD);
	do {
		ssize_t rc = xprt_ssl_read(xprt, buff, sizeof(buff));
		if (rc < 1) {
			fflush(stdout);
			if (rc != -EAGAIN) {
				shutdown_xprt(xprt, SHUT_RDWR);
				log_warn("ssl has been closed");
			}
			return;
		}
		printf("%.*s", (int)rc, buff);
	} while(1);
}

static void client_send(struct xprt *xprt, unsigned long stats)
{
	log_info("writeable");
	ssize_t rc =  xprt_ssl_write(xprt, https_buff, sizeof(https_buff)-1);
	if (WARN_ON(rc!=sizeof(https_buff)-1))
		shutdown_xprt(xprt, SHUT_RDWR);
}

static void client_changed(struct xprt *xprt, unsigned long stats)
{
	if (stats & XPRT_OPENED) {
		log_info("connect success ...");
		/*获取证书信息*/
		X509 *cert;
		char *line;
		struct xprt_ssl *xptssl = xprt_to_ssl(xprt);
		cert = SSL_get_peer_certificate(xptssl->ssl);
		if (cert!=NULL) {
			line = X509_NAME_oneline(X509_get_subject_name(cert), 0, 0);
			printf("cert subject name: %s\n", line);
			(free)(line);
			line = X509_NAME_oneline(X509_get_issuer_name(cert), 0, 0);
			printf("cert issuer name: %s\n", line);
			(free)(line);
			X509_free(cert);
		}
		xprt_event_enable(xprt, EVENT_READ);
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
		.host = "www.okex.com",
		//.host = "127.0.0.1",
		.serv = "443",
	};

	struct xprt * ssl = create_xprt(SRV, &host_addr,
			XPRT_TCPCLNT|XPRT_OPT_NONBLOCK|XPRT_WRREADY,
			&client_ops, (void*)&host_addr);
	BUG_ON(!ssl);

	xprt_put(ssl);

	server_loop(SRV);

	return 0;
}

