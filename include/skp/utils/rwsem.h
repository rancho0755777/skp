//
//  rwsem.h
//
//  Created by 周凯 on 2019/3/4.
//  Copyright © 2019 zhoukai. All rights reserved.
//

#ifndef __US_RWSEM_H__
#define __US_RWSEM_H__

#include "utils.h"
#include "mutex.h"
#include "spinlock.h"
#include "../adt/list.h"

__BEGIN_DECLS
/*
 * the rw-semaphore definition
 * - if activity is 0 then there are no active readers or writers
 * - if activity is +ve then that is the number of active readers
 * - if activity is -1 then there is one active writer
 * - if wait_list is not empty, then there are processes waiting for the semaphore
 */
typedef struct rw_semaphore {
	int activity; /**<
		* - if activity is 0 then there are no active readers or writers
		* - if activity is +ve then that is the number of active readers
		* - if activity is -1 then there is one active writer
		*/
	spinlock_t wait_lock;
	struct list_head	wait_list; /**<
		* if wait_list is not empty, then there are processes waiting for the semaphore*/
} rwsem_t;

#define __RWSEM_INITIALIZER(name)				\
	{	0, __SPIN_LOCK_INITIALIZER(name.wait_lock), \
		LIST_HEAD_INIT((name).wait_list), }

#define DEFINE_RWSEM(name)						\
	struct rw_semaphore name = __RWSEM_INITIALIZER(name)

extern void init_rwsem(struct rw_semaphore *sem);
extern void down_read(struct rw_semaphore *sem);
extern bool down_read_trylock(struct rw_semaphore *sem);
extern void down_write(struct rw_semaphore *sem);
extern bool down_write_trylock(struct rw_semaphore *sem);
extern void up_read(struct rw_semaphore *sem);
extern void up_write(struct rw_semaphore *sem);
/**
 * downgrade a write lock into a read lock
 * - just wake up any readers at the front of the queue
 */
extern void downgrade_write(struct rw_semaphore *sem);

__END_DECLS

#endif /* __US_RWSEM_H__ */
