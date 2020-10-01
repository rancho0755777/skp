#include <skp/utils/bug.h>
#include <skp/utils/filelock.h>

/*
 * 文件锁基础控制
 */
bool file_lockbase(int fd, int cmd, short type, short whence, off_t start, off_t len)
{
	struct flock    lock = {};
	int             flag = -1;

	BUG_ON(fd<0);

	lock.l_len = len;
	lock.l_start = start;
	lock.l_type = type;
	lock.l_whence = whence;

	flag = fcntl(fd, cmd, &lock);
	if (skp_unlikely(flag < 0)) {
		int code = errno;
		if (WARN_ON((code != EACCES) && (code != EAGAIN)))
            return false;
	}

	return flag == 0 ? true : false;
}

/*
 * 文件锁测试
 */
pid_t file_locktest(int fd, short type, short whence, off_t start, off_t len)
{
	struct flock    lock = {};
	int             flag = -1;

	BUG_ON(fd<0);

	lock.l_len = len;
	lock.l_start = start;
	lock.l_whence = whence;
	lock.l_type = type;

	flag = fcntl(fd, F_GETLK, &lock);
	if (skp_unlikely(flag < 0))
        return -1;

	return lock.l_type == F_UNLCK ? 0 : (lock.l_pid);
}