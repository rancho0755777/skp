#ifndef __US_FILE_LOCK_H__
#define __US_FILE_LOCK_H__

#include <fcntl.h>
#include "compiler.h"

__BEGIN_DECLS

/*
 * 文件加锁
 * l_whence : SEEK_SET/SEEK_CUR/SEEK_END 分别表示文件起始/文件当前偏移/文件末尾
 * l_start指定相对于l_whence的起始字节偏移数；l_whence为SEEK_CUR或SEEK_END时，l_start可以指定为负值
 * l_len指定从l_start与l_whence计算得出的偏移值开始，锁定区域的字节长度
 * 
 * 值为0，表示从l_start与l_whence计算得出的偏移值开始，至文件末尾，而不论文件长度的变化
 * 值为正数，表示[l_start, l_start+l_len-1]
 * 值为负数，表示[l_start+l_len, l_start-1]
 * 
 * 注意：不能 flock() 混用，否则加锁、解锁行为将出现不可预期的错误
 */

extern bool file_lockbase(int fd, int cmd, short type, short whence, off_t start, off_t len);
extern pid_t file_locktest(int fd, short type, short whence, off_t start, off_t len);

/*
 * 阻塞式写锁
 */
#define __file_write_lock(fd, whence, start, len) \
	(file_lockbase((fd), F_SETLKW, F_WRLCK, (whence), (start), (len)))

/*
 * 阻塞式读锁
 */
#define __file_read_lock(fd, whence, start, len) \
	(file_lockbase((fd), F_SETLKW, F_RDLCK, (whence), (start), (len)))

/*
 * 非阻塞式写锁
 */
#define __file_write_trylock(fd, whence, start, len)	\
	(file_lockbase((fd), F_SETLK, F_WRLCK, (whence), (start), (len)))

/*
 * 非阻塞式读锁
 */
#define __file_read_trylock(fd, whence, start, len) \
	(file_lockbase((fd), F_SETLK, F_RDLCK, (whence), (start), (len)))

/*
 * 解锁
 */
#define __file_unlock(fd, whence, start, len) \
	(file_lockbase((fd), F_SETLK, F_UNLCK, (whence), (start), (len)))

/*
 * 测试锁是否可写
 * 返回 = 0 表示可写
 * 返回 > 0 表示有进程号为该值的进程持有该锁
 * 返回 = -1 表示出错
 */
#define __file_write_ready(fd, whence, start, len)	\
	(file_locktest((fd), F_WRLCK, (whence), (start), (len)))

/*
 * 测试锁是否可读
 * 返回 = 0 表示可读
 * 返回 > 0 表示有进程号为该值的进程持有该锁
 * 返回 = -1 表示出错
 */
#define __file_read_ready(fd, whence, start, len) \
	(file_locktest((fd), F_RDLCK, (whence), (start), (len)))


#define file_write_lock(fd) __file_write_lock((fd), SEEK_SET, 0, 0)
#define file_read_lock(fd) __file_read_lock((fd), SEEK_SET, 0, 0)
#define file_write_trylock(fd) __file_write_trylock((fd), SEEK_SET, 0, 0)
#define file_read_trylock(fd) __file_read_trylock((fd), SEEK_SET, 0, 0)
#define file_unlock(fd) __file_unlock((fd), SEEK_SET, 0, 0)
#define file_write_ready(fd) __file_write_ready((fd), SEEK_SET, 0, 0)
#define file_read_ready(fd) __file_read_ready((fd), SEEK_SET, 0, 0)


__END_DECLS

#endif