#include <skp/server/xprt.h>
#include <skp/server/server.h>
#include <skp/mm/slab.h>

static inline int init_server(struct server *serv, uint32_t max_xprts,
		uint32_t opt)
{
	serv->nr_xprts = 0;
	serv->destructor = NULL;
	serv->max_xprts = max_xprts;
	serv->flags = opt | SERVER_INITING;

	uref_init(&serv->refs);
	spin_lock_init(&serv->lock);
	INIT_LIST_HEAD(&serv->xprt_list);
	init_waitqueue_head(&serv->waitqueue);

	return 0;
}

static void stop_xprts(struct server *serv, struct list_head *list)
{
	struct xprt *xprt;
	while (!list_empty(list)) {
		xprt = list_first_entry(list, struct xprt, node);
		xprt_get(xprt);
		spin_unlock(&serv->lock);
		destroy_xprt(xprt);
		spin_lock(&serv->lock);
	}
}

static int wait_users(struct server *serv, int timedout)
{
	DEFINE_WAITQUEUE(wait);

	BUG_ON(!uref_read(&serv->refs));

	add_wait_queue_locked(&serv->waitqueue, &wait);
	if (uref_read(&serv->refs) > 1) {
		log_debug("start waiting for server been stopped");
		spin_unlock(&serv->lock);
		wait_on_timeout(&wait, timedout);
		spin_lock(&serv->lock);
	}
	remove_wait_queue_locked(&serv->waitqueue, &wait);

	return uref_read(&serv->refs) > 1 ? -EAGAIN : 0;
}

struct server *___alloc_server(size_t metasize, uint32_t max_xprts, uint32_t opt)
{
	struct server *serv;

	BUG_ON(metasize < sizeof(*serv));

	if (WARN_ON(!max_xprts))
		return NULL;

	WARN_ON(max_xprts < 2);

	serv = malloc(metasize);
	if (skp_unlikely(!serv))
		return NULL;
	
	memset(serv, 0, metasize);
	init_server(serv, max_xprts, opt);

	return serv;
}

void destroy_server(struct server *serv)
{
	uint32_t stats;

	spin_lock(&serv->lock);
	stats = server_set_stats(serv, SERVER_DESTROYED);
	if (WARN_ON(stats == SERVER_DESTROYED)) {
		spin_unlock(&serv->lock);
		return;
	}
	/*check loop*/
	if (WARN_ON(stats == SERVER_RUNNING)) {
		wake_up_all_locked(&serv->waitqueue);
		spin_unlock(&serv->lock);
		sched_yield();
		spin_lock(&serv->lock);
	}

	WARN_ON(server_get_stats(serv) != SERVER_DESTROYED);

	while (wait_users(serv, 1000))
		stop_xprts(serv, &serv->xprt_list);

	log_debug("all of xprt has been destroy");

	BUG_ON(!list_empty(&serv->xprt_list));
	BUG_ON(uref_read(&serv->refs) != 1);

	spin_unlock(&serv->lock);

	if (serv->destructor)
		serv->destructor(serv);

	free(serv);
}

void server_loop(struct server *serv)
{
	uint32_t stats;
	spin_lock(&serv->lock);
	if (__server_has_stopped(serv))
		goto out;
	stats = server_set_stats(serv, SERVER_RUNNING);
	if (WARN_ON(stats == SERVER_RUNNING))
		goto unlock;
	wait_event_lock(&serv->waitqueue,
		__server_has_stopped(serv), &serv->lock);
	stats = server_get_stats(serv);
	if (WARN_ON(stats != SERVER_STOPPING))
		goto unlock;
out:
	server_set_stats(serv, SERVER_STOPPED);
unlock:
	spin_unlock(&serv->lock);
}

void __server_stop(struct server *serv, bool destroy_xprts)
{
	uint32_t stats;

	/*如果还在初始化，则等待*/
	__cond_load_acquire(&serv->flags, VAL != SERVER_INITING);	

	spin_lock(&serv->lock);
	if (WARN_ON(__server_has_stopped(serv)))
		goto unlock;

	if (destroy_xprts)
		stop_xprts(serv, &serv->xprt_list);

	if (WARN_ON(__server_has_stopped(serv)))
		goto unlock;

	stats = server_get_stats(serv);
	if (stats == SERVER_RUNNING) {
		server_set_stats(serv, SERVER_STOPPING);
		wake_up_all_locked(&serv->waitqueue);
	}
unlock:
	spin_unlock(&serv->lock);
}
