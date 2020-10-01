#include <skp/utils/utils.h>
#include <skp/server/socket.h>

int main(void)
{
	char buff[16];
	ssize_t bytes;
    int rc, sfd[2];

    /*创建*/
    rc = socketpair(AF_UNIX, SOCK_STREAM, 0, sfd);
    if (skp_unlikely(rc)) {
        log_error("socketpair failed : %s", strerror_local());
        BUG();
    }

    /*超时读*/
	int timeout = 10;

	bytes = stream_read(sfd[0], buff, ARRAY_SIZE(buff), &timeout);
	BUG_ON(bytes != -ETIMEDOUT);
	BUG_ON(timeout >= 10);

	/*写数据*/
	timeout = 10;
	bytes = stream_write(sfd[1], buff, ARRAY_SIZE(buff), &timeout);
	BUG_ON(bytes!= ARRAY_SIZE(buff));
	BUG_ON(timeout == 0);

	timeout = 10;
	bytes = stream_read(sfd[0], buff, ARRAY_SIZE(buff), &timeout);
	BUG_ON(bytes != ARRAY_SIZE(buff));

	timeout = 10;
	bytes = stream_read(sfd[0], buff, ARRAY_SIZE(buff), &timeout);
	BUG_ON(bytes != -ETIMEDOUT);

	close(sfd[1]);
	timeout = 10;
	bytes = stream_read(sfd[0], buff, ARRAY_SIZE(buff), &timeout);
	BUG_ON(bytes != 0);

	close(sfd[0]);

	/*地址转换*/
	const struct service_address saddr = {
		.host = "localhost",
		.serv = "http"
	};

	struct addrinfo *addrinfo = sock_getaddrinfo(&saddr,
									AI_PASSIVE, AF_UNSPEC, SOCK_STREAM);

	BUG_ON(!addrinfo);

	printf("service_address %s:%s\n", saddr.host, saddr.serv);
	for (struct addrinfo *f = addrinfo; f; f = f->ai_next) {
		struct sock_address saddr;
		struct sock_address tmp;

		printf("\tfamily - %d\n", f->ai_family);
		printf("\ttype - %d\n", f->ai_socktype);
		printf("\tprotocol - %d\n", f->ai_protocol);

		saddr.length = f->ai_addrlen;
		memcpy(&saddr.sock_addr, f->ai_addr, f->ai_addrlen);

		rc = sockaddr_ntop(&saddr, buff, ARRAY_SIZE(buff));
		if (WARN_ON(rc)) {
			log_error("sockaddr_ntop failed : %s", __strerror_local(-rc));
		} else {
			printf("\taddress - %s\n", buff);
		}

		rc = sockaddr_pton(f->ai_family, buff, &tmp);
		if (WARN_ON(rc)) {
			log_error("sockaddr_pton failed : %s", __strerror_local(-rc));
		} else {
			WARN_ON(f->ai_addrlen != tmp.length);
			if (f->ai_family == AF_INET) {
				WARN_ON(memcmp(&saddr.sin_addr.sin_addr,
							   &tmp.sin_addr.sin_addr, 4));
				printf("\tport - %d\n", ntohs(saddr.sin_addr.sin_port));
			} else if (f->ai_family == AF_INET6) {
				WARN_ON(memcmp(&saddr.sin6_addr.sin6_addr,
							   &tmp.sin6_addr.sin6_addr, 4*4));
				printf("\tport - %d\n", ntohs(saddr.sin6_addr.sin6_port));
			}
		}
		printf("---------------------------\n");
	}
	sock_freeaddrinfo(addrinfo);

	/*测试内核自动选择端口*/
	const struct service_address auto_port_addr = {
		.host = "127.0.0.1",
		.serv = "0"
	};

	int auto_port_fd = tcp_listen(&auto_port_addr, 0, 0);
	BUG_ON(auto_port_fd < 0);
	struct sockaddr_in check_sin4;
	socklen_t check_l = sizeof(check_sin4);

	rc = getsockname(auto_port_fd, (void*)&check_sin4, &check_l);
	BUG_ON(rc);
	char *check_ip=(char*)&check_sin4.sin_addr.s_addr;
	printf("ip:%d.%d.%d.%d,port:%d\n", check_ip[0], check_ip[1], check_ip[2],
		check_ip[3], ntohs(check_sin4.sin_port));

	close(auto_port_fd);

	const struct service_address taddr = {
		.host = "localhost",
		.serv = "1024"
	};
	/*地址侦听*/
	int lsnt = tcp_listen(&taddr, 0, 0);
	if (skp_unlikely(lsnt < 0)) {
		log_error("tcp_listen failed : %s", __strerror_local(-lsnt));
		BUG();
	}

	int clnt = tcp_connect(&taddr, 0, 0, 0, 0);
	if (skp_unlikely(lsnt < 0)) {
		log_error("tcp_connect failed : %s", __strerror_local(-clnt));
		BUG();
	}

	int clnt_passive = tcp_accept(lsnt, 0);
	if (skp_unlikely(clnt_passive < 0)) {
		log_error("tcp_accept failed : %s", __strerror_local(-clnt_passive));
		BUG();
	}

	close(lsnt);
	close(clnt);
	close(clnt_passive);

	const struct service_address raddr = {
		.host = "0.0.0.0",
		.serv = "10001"
	};

	/*UDP 测试*/
	int rfd = udp_bind(&raddr);
	if (skp_unlikely(rfd < 0)) {
		log_error("udp_bind failed : %s", __strerror_local(-rfd));
		BUG();
	}

	const struct service_address waddr = {
		.host = "127.0.0.1",
		.serv = "10001"
	};

	int wfd = udp_connect(&waddr);
	if (skp_unlikely(wfd < 0)) {
		log_error("udp_connect failed : %s", __strerror_local(-rfd));
		BUG();
	}

	timeout = 10;
	bytes = dgram_read(rfd, buff, sizeof(buff), &timeout, NULL);
	BUG_ON(bytes != -ETIMEDOUT);
	WARN_ON(timeout);

	bytes = dgram_write(wfd, buff, sizeof(buff), NULL, NULL);
	BUG_ON(bytes != sizeof(buff));

	bytes = dgram_read(rfd, buff, sizeof(buff), NULL, NULL);
	BUG_ON(bytes != sizeof(buff));

	close(rfd);
	close(wfd);

	/*多播测试*/
	const struct service_address m_laddr = {
		.host = "0.0.0.0",
		.serv = "10001"
	};

	const struct service_address m_gaddr = {
		.host = "239.255.1.2",
		.serv = "10001"
	};

	/*绑定地址用于过滤*/
	int m_rfd_01 = udp_bind(&m_laddr);
	BUG_ON(m_rfd_01 < 0);

	int m_rfd_02 = udp_bind(&m_gaddr);
	BUG_ON(m_rfd_02 < 0);

	/*加入多播组*/
	rc = mcast_join(m_rfd_01, &m_gaddr, NULL);
	if (skp_unlikely(rc < 0)) {
		log_error("udp_connect failed : %s", __strerror_local(-rc));
		BUG();
	}

	/*指定接收多播的网卡，以IP地址表示，不能是环回地址？*/
	rc = mcast_join(m_rfd_02, &m_gaddr, NULL);
	if (skp_unlikely(rc < 0)) {
		log_error("mcast_join failed : %s", __strerror_local(-rc));
		BUG();
	}

	/*相当于存储目标地址*/
	int m_wfd_01 = udp_connect(&m_gaddr);
	BUG_ON(m_wfd_01 < 0);

	const struct service_address m_laddr_c = {
		.host = "127.0.0.1",
		.serv = "10001"
	};
	int m_wfd_02 = udp_connect(&m_laddr_c);
	BUG_ON(m_wfd_02 < 0);

	/*如果需要特殊的网卡来发送数据，则设置外出网卡的IP地址*/
	rc = mcast_set_interface(m_wfd_01, NULL);
	if (skp_unlikely(rc < 0)) {
		log_error("mcast_set_interface failed : %s", __strerror_local(-rc));
		BUG();
	}

	bytes = dgram_write(m_wfd_01, buff, sizeof(buff), NULL, NULL);
	BUG_ON(bytes != sizeof(buff));

	bytes = dgram_read(m_rfd_01, buff, sizeof(buff), NULL, NULL);
	BUG_ON(bytes != sizeof(buff));

	bytes = dgram_read(m_rfd_02, buff, sizeof(buff), NULL, NULL);
	BUG_ON(bytes != sizeof(buff));

	bytes = dgram_write(m_wfd_02, buff, sizeof(buff), NULL, NULL);
	BUG_ON(bytes != sizeof(buff));

	bytes = dgram_read(m_rfd_01, buff, sizeof(buff), NULL, NULL);
	BUG_ON(bytes != sizeof(buff));

	timeout = 10;
	bytes = dgram_read(m_rfd_01, buff, sizeof(buff), &timeout, NULL);
	BUG_ON(bytes != -ETIMEDOUT);
	WARN_ON(timeout);

	timeout = 10;
	bytes = dgram_read(m_rfd_02, buff, sizeof(buff), &timeout, NULL);
	BUG_ON(bytes != -ETIMEDOUT);
	WARN_ON(timeout);

    return EXIT_SUCCESS;
}
