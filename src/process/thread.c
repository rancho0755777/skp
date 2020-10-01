#include <signal.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <skp/utils/utils.h>
#include <skp/utils/futex.h>
#include <skp/utils/mutex.h>
#include <skp/process/thread.h>
#include <skp/process/event.h>

#include <skp/mm/slab.h>

#ifdef DEBUG
# define RESERVE_NR_STACK 1
#else
# define RESERVE_NR_STACK NR_CPUS
#endif

static struct _thread main_thread = {
	.flags = 1UL << THREAD_MAINTHREAD_BIT,
	.tgid = 0, .pid = 0, .stack = NULL,
	.started = __COMPLETION_INTIALIZER(main_thread.started),
	.stopped = __COMPLETION_INTIALIZER(main_thread.stopped),
	.pthid = (pthread_t)NULL, .ret = 0,
};

static int nr_stack;
static LIST__HEAD(stack_list);
static DEFINE_MUTEX(stack_mutex);
__thread uthread_t current = &main_thread;

static pthread_key_t tlsclnr_key;
static bool tlsclnr_key_up = false;
static __thread struct list_head *tlsclnr_head = NULL;

struct stack_entry {
#ifdef __linux__
	pid_t pid;
#else
	pthread_t pthid;
	cycles_t exit_ts;
#endif
	void *stack;
	struct list_head node;
};

static inline bool stack__alive(struct stack_entry *entry)
{
	int rc = 1;
#ifdef __linux__
	rc = kill(entry->pid, 0);
	if (!rc)
		rc = 1;
#else
	if (cycles_to_ns(get_cycles() - entry->exit_ts) > RCU_US * 500)
		rc = pthread_kill(entry->pthid, 0);
#endif
	return !!rc;
}

static void *stack__pop(void)
{
	void *stack = NULL;
	struct stack_entry *entry;

	mutex_lock(&stack_mutex);
	list_for_each_entry(entry, &stack_list, node) {
		/*持有该资源的线程仍然存在*/
		if (!stack__alive(entry)) {
			nr_stack--;
			stack = entry->stack;
			list_del(&entry->node);
			free(entry);
			break;
		}
	}
	mutex_unlock(&stack_mutex);

	if (!stack) {
		stack = mmap(NULL, CONFIG_THREAD_STACK,
			PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
		if (WARN_ON(stack == MAP_FAILED)) {
			stack = NULL;
		} else if (WARN_ON(mprotect(stack, PAGE_SIZE, PROT_NONE))) {
			munmap(stack, CONFIG_THREAD_STACK);
			stack = NULL;
		}
	}

	if (skp_likely(stack))
		log_debug("create stack : %p", stack);

	return stack;
}

static void stack__push(void *stack, pid_t pid)
{
	struct stack_entry *entry;

	mutex_lock(&stack_mutex);
	while (nr_stack > RESERVE_NR_STACK) {
		entry = list_first_entry(&stack_list, struct stack_entry, node);
		if (stack__alive(entry))
			break;
		nr_stack--;
		list_del(&entry->node);
		munmap(entry->stack, CONFIG_THREAD_STACK);
		free(entry);
		log_info("release stack : %p", (char*)entry - PAGE_SIZE);
	}

	entry = malloc(sizeof(*entry));
	BUG_ON(!entry);

	nr_stack++;
	entry->stack = stack;
#ifdef __linux__
	entry->pid = pid;
#else
	/*TODO:不是一个完美的解决方案*/
	entry->pthid = pthread_self();
	entry->exit_ts = get_cycles();
#endif
	list_add_tail(&entry->node, &stack_list);
	mutex_unlock(&stack_mutex);
}

static void thread__free(uthread_t thread);

static void *thread__helper(void *stack)
{
	void *arg;
	int rc = 0;
	thread_fn fn;
	uthread_t thread;
	completion_t *done;
	unsigned long *ptr = (unsigned long*)stack;

	/*必须将所有参数存入栈顶的空间*/
	arg = (void*)ptr[0];
	fn = (int(*)(void*))ptr[1];
	thread = (uthread_t)ptr[2];
	done = (completion_t*)ptr[3];

	if (WARN_ON(thread->stack + PAGE_SIZE != stack)) {
		log_error("can't location all of arguments in stack space, "
			"check model of pthread");
		BUG();
	}

	BUG_ON(!fn);
	BUG_ON(!thread);

	/*初始化私有数据*/
	thread->pid = get_thread_id();

	WRITE_ONCE(current, thread);

	/*create success*/
	complete(done);
	wait_for_completion(&thread->started);

	if (!uthread_should_stop()) {
		BUG_ON(test_and_set_bit(THREAD_RUNNING_BIT, &thread->flags));
		thread->ret = fn(arg);
	}

	if (test_and_set_bit(THREAD_STOPPED_BIT, &thread->flags))
		rc = -EINTR;

	complete(&thread->stopped);
	WRITE_ONCE(current, NULL);

	if (test_bit(THREAD_DETACHED_BIT, &thread->flags)) {
		thread__free(thread);
		pthread_exit((void*)(intptr_t)rc);
	}

	while (rc) {
		pthread_testcancel();
		sleep(1);
	}

	pthread_exit((void*)(intptr_t)rc);
}

static uthread_t thread__new(thread_fn fn, void *arg, size_t classize,
		completion_t *done)
{
	unsigned long *ptr = 0;
	uthread_t thread = malloc(classize);

	BUG_ON(!fn);
	BUG_ON(classize < sizeof(*thread));

	if (skp_unlikely(!thread))
		return NULL;

	thread->pid = -1;
	thread->flags = 0;
	thread->stack = NULL;
	thread->tgid = getpid();
	thread->pthid = (pthread_t)NULL;

	init_completion(&thread->started);
	init_completion(&thread->stopped);
	thread->stack = stack__pop();
	if (skp_unlikely(!thread->stack))
		goto out_free;

	ptr = (unsigned long *)(thread->stack + PAGE_SIZE);
	ptr[0] = (unsigned long)arg;
	ptr[1] = (unsigned long)fn;
	ptr[2] = (unsigned long)thread;
	ptr[3] = (unsigned long)done;

	return thread;

out_free:
	free(thread);
	return NULL;
}

static void thread__free(uthread_t thread)
{
	BUG_ON(!test_bit(THREAD_STOPPED_BIT, &thread->flags));
	if (thread->stack)
		stack__push(thread->stack, thread->pid);
	__cond_load_acquire(&thread->flags, !(VAL & THREAD_WAKING));
	free(thread);
}

uthread_t __uthread_create(thread_fn fn, void *arg, size_t classize)
{
	int rc = 0;
	uthread_t thread;
	pthread_attr_t attr;
	DEFINE_COMPLETION(done);

	rc = pthread_attr_init(&attr);
	if (skp_unlikely(rc))
		return NULL;

	thread = thread__new(fn, arg, classize, &done);
	if  (skp_unlikely(!thread))
		goto out_attr;

	rc = pthread_attr_setstack(&attr, thread->stack, CONFIG_THREAD_STACK);
	if (skp_unlikely(rc))
		goto out_free;

	rc = pthread_create(&thread->pthid, &attr, thread__helper,
				thread->stack + PAGE_SIZE);
	if (skp_unlikely(rc))
		goto out_free;

	wait_for_completion(&done);

	log_debug("create new thread : %lx", (unsigned long)thread->pthid);
	goto out_attr;

out_free:
	__set_bit(THREAD_STOPPED_BIT, &thread->flags);
	thread__free(thread);
	thread = NULL;
out_attr:
	pthread_attr_destroy(&attr);

	return thread;
}

int uthread_wakeup(uthread_t thread)
{
	int rc = -EBUSY;

	if (skp_unlikely(!thread))
		return -EINVAL;
	if (test_and_set_bit(THREAD_WAKING_BIT, &thread->flags))
		return -EBUSY;
	if (READ_ONCE(thread->flags) & THREAD_RUNNING)
		goto out;

	complete(&thread->started);
	/* 必须等待此标记，否则随后立马调用 uthread_stop()
	 * 线程就退出了， 这与 uthread_wakeup() 语义冲突了
	 */
	__cond_load_acquire(&thread->flags,
		(VAL & THREAD_RUNNING) || (VAL & THREAD_STOPPING));

	rc = 0;
out:
	test_and_clear_bit(THREAD_WAKING_BIT, &thread->flags);
	return rc;
}

uthread_t uthread_run(thread_fn fn, void *arg)
{
	uthread_t thread = uthread_create(fn, arg);
	if (thread)
		uthread_wakeup(thread);
	return thread;
}

static int __thread_wait_join(uthread_t thread)
{
	int rc, pid = (int)thread->pid;
	wait_for_completion(&thread->stopped);
	WARN_ON(!(READ_ONCE(thread->flags) & THREAD_STOPPED));
	pthread_join(thread->pthid, NULL);
	rc = thread->ret;
	thread__free(thread);
	(void)pid;
	log_debug("thread join: %d", pid);
	return rc;
}

int uthread_stop(uthread_t thread, int *exit_code)
{
	int rc;

	BUG_ON(thread == current);
	BUG_ON(test_bit(THREAD_DETACHED_BIT, &thread->flags));
	if (skp_unlikely(!thread) ||
			skp_unlikely(READ_ONCE(thread->flags) & THREAD_MAINTHREAD) ||
			test_and_set_bit(THREAD_STOPPING_BIT, &thread->flags))
		return -EINVAL;

	if (!(READ_ONCE(thread->flags) & THREAD_RUNNING))
		complete(&thread->started);

	rc = __thread_wait_join(thread);
	if (exit_code)
		*exit_code = rc;
	return 0;
}

int uthread_kill(uthread_t thread)
{
	BUG_ON(thread == current);
	BUG_ON(test_bit(THREAD_DETACHED_BIT, &thread->flags));
	if (skp_unlikely(!thread) ||
			skp_unlikely(READ_ONCE(thread->flags) & THREAD_MAINTHREAD) ||
			test_and_set_bit(THREAD_STOPPING_BIT, &thread->flags))
		return -EINVAL;

	if (!(READ_ONCE(thread->flags) & THREAD_RUNNING))
		complete(&thread->started);

	if (!test_and_set_bit(THREAD_STOPPED_BIT, &thread->flags)) {
		pthread_cancel(thread->pthid);
		pthread_join(thread->pthid, NULL);
		thread__free(thread);
		return 1;
	}

	__thread_wait_join(thread);

	return 0;
}

void uthread_detach(void)
{
	BUG_ON(current->flags & THREAD_ISEVENTWORKER);
	if (skp_unlikely(current->flags &
			(THREAD_MAINTHREAD|THREAD_DETACHED)))
		return;
	if (WARN_ON(test_and_set_bit(THREAD_DETACHED_BIT, &current->flags)))
		return;
	pthread_detach(current->pthid);
}

struct tlsclnr_entry {
	void *data;
	tls_cleaner action;
	struct list_head node;
};

#define node2entry(p) container_of((p), struct tlsclnr_entry, node)

static void tlsclnr_invoke(void* ptr)
{
	struct list_head *head = ptr;
	struct tlsclnr_entry *clnr, *next;

	if (skp_unlikely(!ptr))
		return;

	list_for_each_entry_safe(clnr, next, head, node) {
		list_del(&clnr->node);
		clnr->action(clnr->data);
		tls_free(clnr);
	}

	tls_free(ptr);
}

static void tlsclnr_init(void)
{
	struct list_head *head;

	if (skp_likely(READ_ONCE(tlsclnr_key_up)))
		goto done;

	big_lock();
	if (!tlsclnr_key_up) {
		BUG_ON(pthread_key_create(&tlsclnr_key, tlsclnr_invoke));
		smp_wmb();
		WRITE_ONCE(tlsclnr_key_up, true);
	}
	big_unlock();

done:
	if (skp_likely(tlsclnr_head))
		return;

	/*TODO:中断带来递归*/
	head = pthread_getspecific(tlsclnr_key);
	WARN_ON(head);
	if (skp_likely(!head)) {
		head = malloc(sizeof(*head));
		BUG_ON(!head);
		INIT_LIST_HEAD(head);
		BUG_ON(pthread_setspecific(tlsclnr_key, head));
	}
	tlsclnr_head = head;
}

void tlsclnr_register(tls_cleaner action, void *data)
{
	struct tlsclnr_entry *clnr;

	tlsclnr_init();

	clnr = malloc(sizeof(*clnr));
	BUG_ON(!clnr);

	clnr->data = data;
	clnr->action = action;
	list_add(&clnr->node, tlsclnr_head);
}
