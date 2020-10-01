#ifndef __SU_SIGNAL_H__
#define __SU_SIGNAL_H__

#include <signal.h>

__BEGIN_DECLS

typedef void (*signal_fn)(int signo);

/*
 * 安装一个信号，除闹铃信号，其他信号将重启被中断的系统调用（原始系统调用）
 */
extern signal_fn signal_setup(int signo, signal_fn cb);

/*
 * 安装一个信号，任何使用都不重启被中断的系统调用（原始系统调用）
 */
extern signal_fn signal_intr_setup(int signo, signal_fn cb);

/*
 * 阻塞所有信号，返回原信号集
 */
extern void signal_block_all(sigset_t *old);

/*
 * 解除阻塞所有信号或设置为指定信号集
 */
extern void signal_unblock_all(const sigset_t *old);

extern int signal_block_one(int signo, sigset_t *old);

extern int signal_unblock_one(int signo);

extern void signal_default(int signo);

__END_DECLS

#endif