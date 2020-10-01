//
//  daemon.c
//  work
//
//  Created by 周凯 on 18/6/11.
//
//
#include <getopt.h>
#include <fcntl.h>
#include <signal.h>
#include <syslog.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/resource.h>

#include <skp/utils/utils.h>
#include <skp/process/daemon.h>

#define PIDFILE "/tmp/%s.daemon.pid"
#define PIDFILE_MODE (S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH)

bool isdaemon = false;

static int already_running(const char *cmd)
{
	ssize_t rc;
	int fd = -1;
	long pid = (long)getpid();
	char *p, strpid[32], path[128];

	p = strrchr(cmd, '/');
	if (p)
		cmd = p + 1;
	BUG_ON(!(*cmd));
	snprintf(path, sizeof(path), PIDFILE, cmd);
	fd = open(path, O_CREAT | O_RDWR, PIDFILE_MODE);
	if (skp_unlikely(fd < 0)) {
		__log_error("%s can't open %s : %s", p, path, strerror(errno));
		exit(1);
	}

	rc = flock(fd, LOCK_EX|LOCK_NB);
	if (skp_unlikely(rc < 0)) {
		if (errno == EWOULDBLOCK) {
			rc = read(fd, strpid, sizeof(strpid));
			if (skp_unlikely(rc < 0)) {
				__log_error("%s can't read file %s", p, path);
				exit(1);
			}
			sscanf(strpid, "%ld", &pid);
			__log_warn("%s daemon already running, pid : %ld", p, pid);
			return -1;
		}
		__log_error("%s can't lock file %s", p, path);
		exit(1);
	}

	ftruncate(fd, 0);
	snprintf(strpid, sizeof(strpid), "%ld", pid);
	write(fd, strpid, strlen(strpid) + 1);
	return fd;
}

int daemonize(int argc, char *argv[])
{
	pid_t pid;
	const char *cmd;
	struct rlimit rl;
	int i, fd0, fd1, fd2, lfd, rc, opt;
	struct option long_opts[] = {
		{ "daemon", no_argument, NULL, 'D' },
		{ NULL, 0, NULL, 0 }
	};

	cmd = strrchr(argv[0], '/');
	cmd = cmd ? cmd + 1 : argv[0];

	optind = 0;
	while ((opt = getopt_long(argc, argv, ":D", long_opts, NULL)) != -1) {
		switch (opt) {
		case 'D':
			isdaemon = true;
			break;
		default:
			break;
		}
	}

	if (!isdaemon) {
		goto nondaemon;
	}

	umask(0);

	rc = getrlimit(RLIMIT_NOFILE, &rl);
	if (skp_unlikely(rc < 0)) {
		log_error("%s can't get file limit : %s", cmd, strerror(errno));
		exit(1);
	}

	pid = fork();
	if (skp_unlikely(pid < 0)) {
		log_error("%s can't fork : %s", cmd, strerror(errno));
		exit(1);
	} else if (pid > 0)
		exit(0);

	setsid();
	signal(SIGHUP, SIG_IGN);

	pid = fork();
	if (skp_unlikely(pid < 0)) {
		log_error("%s can't fork : %s", cmd, strerror(errno));
		exit(1);
	} else if (pid > 0)
		exit(0);

	rc = chdir("/");
	if (skp_unlikely(rc < 0)) {
		log_error("%s can't change directory to `/` : %s", cmd, strerror(errno));
		exit(1);
	}

	if (rl.rlim_max == RLIM_INFINITY) {
		rl.rlim_max = 1024;
	}

	for (i = 0; i < rl.rlim_max; i++) {
		close(i);
	}

	fd0 = open("/dev/null", O_RDWR);
	fd1 = dup2(fd0, 1);
	fd2 = dup2(fd0, 2);
	openlog(cmd, LOG_CONS, LOG_DAEMON);

	if (fd0 != 0 || fd1 != 1 || fd2 != 2) {
		__log_error("unexpected file descriptors %d %d %d", fd0, fd1, fd2);
		exit(1);
	}
	signal(SIGHUP, SIG_DFL);
nondaemon:

	lfd = already_running(cmd);
	if (skp_unlikely(lfd < 0)) {
		return -1;
	}

	return lfd;
}
