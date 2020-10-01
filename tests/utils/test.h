#ifndef SKP_TEST_H
#define SKP_TEST_H

#include <pthread.h>

static inline pthread_t thread_create(void *(*cb)(void*), void *arg)
{
	pthread_t pthd;
	int rc = pthread_create(&pthd, NULL, cb, arg);
	return rc ? (pthread_t)NULL : pthd;
}

static inline void thread_join(pthread_t pthd)
{
	pthread_join(pthd, NULL);
}


#endif