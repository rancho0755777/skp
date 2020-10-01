#ifndef __US_COMPLETION_H__
#define __US_COMPLETION_H__

#include "wait.h"

__BEGIN_DECLS

typedef struct __completion completion_t;

struct __completion {
	int32_t done;
	wait_queue_head_t wait_queue;
};

#define __COMPLETION_INTIALIZER(x) \
	{ .done = 0, .wait_queue = __WAIT_QUEUE_HEAD_INITIALIZER((x).wait_queue), }

#define DEFINE_COMPLETION(name) \
	completion_t name = __COMPLETION_INTIALIZER(name)

/*use to re-intialize*/
#define INIT_COMPLETION(x) (WRITE_ONCE((x).done, 0))

static inline void init_completion(completion_t *x)
{
	x->done = 0;
	init_waitqueue_head(&x->wait_queue);
}

extern void __complete(completion_t *x, int nr);
/**@return -1 error , 0 timedout , 1 success*/
extern int wait_for_completion_timeout(completion_t *, int);

#define complete(x) __complete((x), 1)
#define complete_all(x) __complete((x), UINT_MAX >> 1)
#define wait_for_completion(x) wait_for_completion_timeout((x), -1)

static inline bool try_wait_for_completion(completion_t *x)
{
	return wait_for_completion_timeout(x, 0) > 0 ? true : false;
}

__END_DECLS

#endif
