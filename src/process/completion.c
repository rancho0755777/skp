#include <skp/process/completion.h>

void __complete(completion_t *x, int nr)
{
	spin_lock(&x->wait_queue.lock);
	x->done += nr;
	__wake_up_locked(&x->wait_queue, nr, NULL);
	spin_unlock(&x->wait_queue.lock);
}

int wait_for_completion_timeout(completion_t *x, int timeout)
{
	int rc = 1;
	DEFINE_AUTOREMOVE_WAITQUEUE(wait);

	spin_lock(&x->wait_queue.lock);
	if (READ_ONCE(x->done))
		goto done;
	add_wait_queue_exclusive_locked(&x->wait_queue, &wait);
	do {
		spin_unlock(&x->wait_queue.lock);
		rc = wait_on_timeout(&wait, timeout);
		spin_lock(&x->wait_queue.lock);
		if (rc < 1) {
			/*timedout*/
			remove_wait_queue_locked(&x->wait_queue, &wait);
			goto out;
		}
	} while (!READ_ONCE(x->done));
	remove_wait_queue_locked(&x->wait_queue, &wait);
done:
	x->done -= 1;
out:
	spin_unlock(&x->wait_queue.lock);
	return rc;
}
