#include <sys/cdefs.h>
#include <skp/utils/utils.h>
#include <skp/utils/mutex.h>
#include <skp/utils/rwlock.h>
#ifdef __apple__
#include <sys/event.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#else
#include <sys/inotify.h>
#endif
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifndef CONFIG_ERRMSG_SIZE
# define CONFIG_ERRMSG_SIZE 64
#endif

static __cacheline_aligned DEFINE_RECURSIVE_MUTEX(__big_lock);

void big_lock(void)
{
	recursive_mutex_lock(&__big_lock);
}

void big_unlock(void)
{
	recursive_mutex_unlock(&__big_lock);
}

const char *__strerror_local(int code)
{
	static __thread char __errmsg[CONFIG_ERRMSG_SIZE];

	BUILD_BUG_ON(CONFIG_ERRMSG_SIZE < 64);
	if (skp_unlikely(code < 0)) {
unkown:
		snprintf(__errmsg, sizeof(__errmsg),
			"Unkown error [%d], or call strerror_r() failed", code);
	} else {
#if ((_POSIX_C_SOURCE >= 200112l || _XOPEN_SOURCE >= 600) && !_GNU_SOURCE) || !defined(__linux__)
		int rc;
		rc = strerror_r(code, __errmsg, sizeof(__errmsg));
		if (skp_unlikely(rc < 0))
			goto unkown;
#else
		char *rc;
		rc = strerror_r(code, __errmsg, sizeof(__errmsg));
		if (skp_unlikely(!rc))
			goto unkown;
		/*syscall bug*/
		snprintf(__errmsg, sizeof(__errmsg), "%s", rc);
#endif
	}

	return __errmsg;
}
////////////////////////////////////////////////////////////////////////////////
struct rnd_state {
	uint32_t s1, s2, s3, s4;
};

static __always_inline uint32_t prandom_u32_state(struct rnd_state *state)
{
#define TAUSWORTHE(s, a, b, c, d) ((s & c) << d) ^ (((s << a) ^ s) >> b)
	state->s1 = TAUSWORTHE(state->s1,  6U, 13U, 4294967294U, 18U);
	state->s2 = TAUSWORTHE(state->s2,  2U, 27U, 4294967288U,  2U);
	state->s3 = TAUSWORTHE(state->s3, 13U, 21U, 4294967280U,  7U);
	state->s4 = TAUSWORTHE(state->s4,  3U, 12U, 4294967168U, 13U);

	return (state->s1 ^ state->s2 ^ state->s3 ^ state->s4);
}

/*
 * Handle minimum values for seeds
 */
static inline uint32_t __seed(uint32_t x, uint32_t m)
{
	return (x < m) ? x + m : x;
}

/**
 * prandom_seed_state - set seed for prandom_u32_state().
 * @state: pointer to state structure to receive the seed.
 * @seed: arbitrary 64-bit value to use as a seed.
 */
static inline void prandom_seed_state(struct rnd_state *state, uint64_t seed)
{
	uint32_t i = (uint32_t)((seed >> 32) ^ (seed << 10) ^ seed);

	state->s1 = __seed(i,   2U);
	state->s2 = __seed(i,   8U);
	state->s3 = __seed(i,  16U);
	state->s4 = __seed(i, 128U);
}

static void prandom_seed_early(struct rnd_state *state, uint32_t seed)
{
#define HWSEED() (random())
#define LCG(x)	 ((x) * 69069U)	/* super-duper LCG */
	state->s1 = __seed((uint32_t)(HWSEED() ^ LCG(seed)),        2U);
	state->s2 = __seed((uint32_t)(HWSEED() ^ LCG(state->s1)),   8U);
	state->s3 = __seed((uint32_t)(HWSEED() ^ LCG(state->s2)),  16U);
	state->s4 = __seed((uint32_t)(HWSEED() ^ LCG(state->s3)), 128U);
}

static void prandom_warmup(struct rnd_state *state)
{
	/* Calling RNG ten times to satisfy recurrence condition */
	prandom_u32_state(state);
	prandom_u32_state(state);
	prandom_u32_state(state);
	prandom_u32_state(state);
	prandom_u32_state(state);
	prandom_u32_state(state);
	prandom_u32_state(state);
	prandom_u32_state(state);
	prandom_u32_state(state);
	prandom_u32_state(state);
}

static __thread struct rnd_state rnd_state = { .s1 = 0, .s2 = 0 };

static __always_inline void prandom_init(void)
{
	struct timespec ts;
	uint64_t weak_seed;

	if (skp_unlikely(!rnd_state.s1 && !rnd_state.s2)) {
		if (skp_unlikely(rnd_state.s1 && rnd_state.s2)) {
			return;
		}
		srandom((uint32_t)time(0));
		get_timestamp(&ts);
		weak_seed = (uint64_t)((get_thread_id() +
			ts.tv_sec) ^ (ts.tv_nsec / 1000));

		weak_seed = (weak_seed & U32_MAX) ^ (weak_seed >> 32);
		prandom_seed_early(&rnd_state, (uint32_t)weak_seed);
		prandom_warmup(&rnd_state);
	}
}

void prandom_seed(uint64_t seed)
{
	prandom_init();
	prandom_seed_state(&rnd_state,
		(HWSEED() ^ (seed & U32_MAX)) | ((HWSEED() ^ (seed >> 32)) << 32));
}

uint32_t prandom(void)
{
	prandom_init();
	return prandom_u32_state(&rnd_state);
}

#include <sys/syscall.h>
int get_thread_id(void)
{
	long tid;
	static __thread int __tid = -1;

	if (skp_unlikely((tid=READ_ONCE(__tid)) == -1)) {
#if defined(__linux__)
		tid = syscall(SYS_gettid);
#elif defined(__apple__)
		tid = syscall(SYS_thread_selfid);
#else
#endif
		BUG_ON(tid < 0);
		WRITE_ONCE(__tid, (int)tid);
	}
	return tid;
}

int get_process_id(void)
{
	long pid;
	static int __pid = -1;

	if (skp_unlikely((pid = READ_ONCE(__pid)) == -1)) {
		pid = getpid();
		BUG_ON(pid < 0);
		WRITE_ONCE(__pid, (int)pid);
	}

	return pid;
}

int get_cpu_cores(void)
{
	long cores;
	static int __cores = -1;

	if (skp_unlikely((cores=READ_ONCE(__cores)) == -1)) {
		cores = sysconf(_SC_NPROCESSORS_CONF);
		BUG_ON(cores < 0);
		WRITE_ONCE(__cores, (int)cores);
	}

	return cores;
}

#include <signal.h>
#include <sys/select.h>
#include <skp/process/thread.h>

void usleep_unintr(uint32_t usecs)
{
	if (!usecs)
		return;

	sigset_t _new_;
	struct timespec ts = {
		.tv_sec = usecs / 1000000,
		.tv_nsec = (usecs % 1000000) * 1000,
	};
	sigfillset(&_new_);
	wq_worker_sleeping();
	pselect(1, 0, 0, 0, &ts, &_new_);
	wq_worker_waking_up();
}

#ifndef CONFIG_CLOCKFREQ /*提供常量时钟频率( MHZ，即每秒 tick 个数)，提高精度和效率*/

static __always_inline uint32_t ___get_clockfreq(void)
{
	/*休眠时间不能过小，否则系统调用的耗时将带来很大的误差*/
	uint32_t wait = 200, clockfreq = 0;
	unsigned long long tsc1, tsc2;

	enter_atomic();
	while (clockfreq < 1024 && wait++ < 1000) {
		tsc1 = rdtscll();
		msleep_unintr(wait);
		tsc2 = rdtscll();
		clockfreq = ((uint32_t)(tsc2 - tsc1)/(wait * 1000));
	}
	leave_atomic();
	return clockfreq;
}

static __always_inline uint32_t __get_clockfreq(void)
{
	const uint32_t AVG_SHIFT = 3;
	static uint32_t __clockfreq = -1U;
	uint32_t avg = -1U, clockfreq = 0;

	if (skp_likely(READ_ONCE(__clockfreq) != -1U))
		return __clockfreq;

	for (int i = 0; i < (1U << AVG_SHIFT); i++) {
		clockfreq = ___get_clockfreq();
		avg += clockfreq;
	}
	avg /= (1U << AVG_SHIFT);
#ifdef DEBUG
	printf("clockfreq on this machine : %u\n", avg);
#endif
	WRITE_ONCE(__clockfreq, avg);
 	return avg;
}
#else
static __always_inline uint32_t __get_clockfreq(void)
{
	BUILD_BUG_ON(CONFIG_CLOCKFREQ < 1);
	return CONFIG_CLOCKFREQ;
}
#endif

uint32_t get_clockfreq(void)
{
	return __get_clockfreq();
}

const char * get_timestamp_string(void)
{
	struct timespec ts;
	static struct my_timespec {
		long tv_sec;
		long tv_nsec;
	} __aligned_double_cmpxchg nowts_start = { -1, -1 };
	static __thread char nowts_string[32];

	get_similar_timestamp(&ts);

	if (skp_unlikely(nowts_start.tv_sec == -1) ||
			skp_unlikely(nowts_start.tv_sec > ts.tv_sec)) {
		long old_sec = READ_ONCE(nowts_start.tv_sec);
		long old_nsec = READ_ONCE(nowts_start.tv_nsec);
		/*由于 64位时间的关系，可能有无，需要确认*/
		cmpxchg_double(&nowts_start.tv_sec, &nowts_start.tv_nsec,
			old_sec, old_nsec, ts.tv_sec, ts.tv_nsec);
	}

	ts.tv_sec -= READ_ONCE(nowts_start.tv_sec);
	ts.tv_nsec -= READ_ONCE(nowts_start.tv_nsec);

	if (ts.tv_nsec < 0) {
		if (ts.tv_sec)
			ts.tv_sec -= 1;
		ts.tv_nsec += 1000 * 1000 * 1000;
	}

	snprintf(nowts_string, sizeof(nowts_string), "%10ld.%09u",
		(long)ts.tv_sec, (uint32_t)ts.tv_nsec);
	return nowts_string;
}

struct timespec *get_timestamp(struct timespec *ts)
{
	static __thread struct timespec __timespec;
	ts = ts?:&__timespec;
#ifdef __linux__
	clock_gettime(CLOCK_MONOTONIC, ts);
#else
    uint64_t ticks;
	static __thread mach_timebase_info_data_t __tb;
	if (__tb.denom == 0)
		mach_timebase_info(&__tb);

	ticks = mach_absolute_time();
	ticks = ticks * __tb.numer / __tb.denom;
	ts->tv_sec = ticks / 1000000000;
	ts->tv_nsec = ticks % 1000000000;
#endif
	return ts;
}

#define MAX_TICKS_DEVIATION(freq) (freq << 18)

struct timespec * get_similar_timestamp(struct timespec *ts)
{
	unsigned long long nowtick;
	static __thread unsigned long long __walltick = 0;
	const unsigned long long clockfreq = __get_clockfreq();
	static __thread struct timespec __timespec, __walltime;

	ts = ts?:&__timespec;
try:
	if (skp_unlikely(__walltick == 0)) {
		__walltick = rdtscll();
		get_timestamp(&__walltime);
	}

	nowtick = rdtscll();

	/* TSC偏移大于这个值，就需要重新获得系统时间 */
	if (skp_unlikely((nowtick - __walltick) >
			MAX_TICKS_DEVIATION(clockfreq))) {
		__walltick = 0;
		goto try;
	}

	/*获取微秒单位的增量*/
	nowtick = (nowtick - __walltick) / clockfreq;

	*ts = __walltime;
	ts->tv_sec += nowtick / 1000000;
	ts->tv_nsec += (nowtick % 1000000) * 1000;
	if (ts->tv_nsec >= 1000000000) {
		ts->tv_nsec -= 1000000000;
		ts->tv_sec += 1;
	}

	return ts;
}

struct timespec * get_calendar(struct timespec *ts)
{
	struct timeval tv;
	static __thread struct timespec __timespec;
	ts = ts?ts:&__timespec;
	gettimeofday(&tv, NULL);
	ts->tv_sec = tv.tv_sec;
	ts->tv_nsec = tv.tv_usec * 1000;
	return ts;
}

struct timespec * get_similar_calendar(struct timespec *ts)
{
	unsigned long long nowtick;
	static __thread unsigned long long __walltick = 0;
	const unsigned long long clockfreq = __get_clockfreq();
	static __thread struct timespec __timespec, __walltime;

	ts = ts?ts:&__timespec;
try:
	if (skp_unlikely(__walltick == 0)) {
		__walltick = rdtscll();
		get_calendar(&__walltime);
	}

	nowtick = rdtscll();

	if (skp_unlikely((nowtick - __walltick) >
			MAX_TICKS_DEVIATION(clockfreq))) {
		__walltick = 0;
		goto try;
	}

	nowtick = (nowtick - __walltick) / clockfreq;

	*ts = __walltime;
	ts->tv_sec += nowtick / 1000000;
	ts->tv_nsec += (nowtick % 1000000) * 1000;
	if (ts->tv_nsec >= 1000000000) {
		ts->tv_nsec -= 1000000000;
		ts->tv_sec += 1;
	}

	return ts;
}

/**
 * 格式时间
 */
char *format_time(char *tm_str, size_t size, time_t tm_int, const char *format)
{
	struct tm local;
	struct tm *tmptr;
	size_t nbytes;
	static __thread char buff[64];

	local.tm_isdst = 0;
	tmptr = localtime_r((time_t *)&tm_int, &local);

	if (skp_unlikely(!tmptr)) {
		log_error("can't formate time [%ld] : %s.",
			tm_int, strerror_local());
		return NULL;
	}

	if (!tm_str) {
		tm_str = buff;
		size = sizeof(buff);
	}

	nbytes = strftime(tm_str, size, format, &local);

	if (skp_unlikely(nbytes == 0)) {
		errno = EINVAL;
		log_error("can't formate time [%ld] : %s.",
			tm_int, strerror_local());
		return NULL;
	}

	return tm_str;
}

extern char *strptime(const char *, const char *, struct tm *);

time_t parse_time(const char *tm_str, const char *format)
{
	time_t result = -1;
	struct tm local;
	char *ptr = NULL;
	ptr = strptime(tm_str, format, &local);
	if (skp_unlikely(!ptr)) {
		log_error("can't parse the string of time [%s] : %s.",
			tm_str, strerror_local());
		return -1;
	}
	local.tm_isdst = 0;
	result = mktime(&local);
	if (skp_unlikely(result == -1)) {
		errno = EINVAL;
		log_error("can't parse the string of time [%s] : %s.",
			tm_str, strerror_local());
	}
	return result;
}

int enable_fd_flag(int fd, int flags)
{
	int rc, oflags;
	if (skp_unlikely(fd < 0))
		return -EBADF;
	oflags = fcntl(fd, F_GETFL, NULL);
	if (skp_unlikely(oflags < 0))
		return -errno;
	rc = fcntl(fd, F_SETFL, oflags | flags);
	if (skp_unlikely(rc < 0))
		return -errno;
	return 0;
}

int disable_fd_flag(int fd, int flags)
{
	int rc, oflags;
	if (skp_unlikely(fd < 0))
		return -EBADF;

	oflags = fcntl(fd, F_GETFL, NULL);
	if (skp_unlikely(oflags < 0))
		return -errno;
	rc = fcntl(fd, F_SETFL, oflags & ~flags);
	if (skp_unlikely(rc < 0))
		return -errno;
	return 0;
}

#ifdef __apple__
int pipe2(int fd[2], int flags)
{
	if (pipe(fd))
		return -1;

	if (flags & O_NONBLOCK) {
		if (set_fd_nonblock(fd[0]))
			goto fail;
		if (set_fd_nonblock(fd[1]))
			goto fail;
	}
	if (flags & O_CLOEXEC) {
		if (set_fd_cloexec(fd[0]))
			goto fail;
		if (set_fd_cloexec(fd[1]))
			goto fail;
	}
	return 0;
fail:
	close(fd[0]);
	close(fd[1]);
	return -1;
}
#endif

int notifier_register(struct notifier **list, struct notifier *n, bool stack)
{
	struct notifier **head = list;

	WARN_ON(n->next != NOTIFY_MAGIC);

	while (*list) {
		if (skp_unlikely(*list == n))
			return -EEXIST;
		list= &((*list)->next);
	}

	/*以栈的方式注册*/
	if (stack)
		list = head;

	n->next = *list;
	*list=n;
	return 0;
}

int notifier_unregister(struct notifier **list, struct notifier *n)
{
	WARN_ON(n->next == NOTIFY_MAGIC);
	while (*list) {
		if (*list==n) {
			*list=n->next;
			return 0;
		}
		list=&(*list)->next;
	}
	return -ENOENT;
}

int notifier_invoke(struct notifier **list, void *user)
{
	int rc = 0;
	struct notifier *c = *list;

	while ((*list)!=NULL) {
		c = *list;
		*list = c->next;
		rc = c->action(c, user);
		if (rc)
			break;
	}

	return rc;
}

#ifdef __apple__

int file_changed(int fd, int timeout)
{
	struct kevent kev;
	int n, kfd, ev = fchg_failed;
	struct timespec ts, *pts = NULL;

	if (timeout > -1) {
		ts.tv_sec = timeout / 1000;
		ts.tv_nsec = (timeout % 1000) * 1000 * 1000;
		pts = &ts;
	}

	/*创建*/
	kfd = kqueue();
	if (skp_unlikely(kfd < 0))
		return fchg_failed;

	EV_SET(&kev, fd, EVFILT_VNODE, EV_ADD|EV_ONESHOT,
		NOTE_WRITE|NOTE_ATTRIB|NOTE_RENAME|NOTE_DELETE|NOTE_EXTEND, 0, NULL);

	/*添加*/
	if (kevent(kfd, &kev, 1, NULL, 0, NULL) < 0) {
		close(kfd);
		return fchg_failed;
	}

	/*等待*/
	n = kevent(kfd, NULL, 0, &kev, 1, pts);

	close(kfd);

	/*分析结果*/
	if (skp_unlikely(n < 0))
		return fchg_failed;

	if (!n) {
		errno = ETIMEDOUT;
		return fchg_failed;
	}

	ev = 0;
	BUG_ON(n!=1);
	BUG_ON(kev.filter!=EVFILT_VNODE);

	if (kev.fflags&NOTE_WRITE)
		ev|=fchg_write;
	if (kev.fflags&NOTE_ATTRIB)
		ev|=fchg_attrib;
	if (kev.fflags&NOTE_RENAME)
		ev|=fchg_rename;
	if (kev.fflags&NOTE_DELETE)
		ev|=fchg_delete;
	if (kev.fflags&NOTE_EXTEND)
		ev|=fchg_extend;

	return ev;
}
#else

static int handle_inotify(int fd, int wd)
{
	int ev = 0;
	const struct inotify_event *event;
	char buf[576] __aligned(__alignof__(struct inotify_event));

	/* Loop while events can be read from inotify file descriptor. */
	for (;;) {
		/* Read some events. */
		ssize_t len = read(fd, buf, sizeof(buf));
		if (len < 1) {
			/* If the nonblocking read() found no events to read, then
			 it returns -1 with errno set to EAGAIN. In that case,
			 we exit the loop. */
			if (skp_unlikely(len && errno != EAGAIN))
				return fchg_failed;
			break;
		}

		/* Loop over all events in the buffer */
		for (const char *ptr = buf; ptr < buf + len;
				ptr += sizeof(struct inotify_event) + event->len) {
			event = (const struct inotify_event *)ptr;
			BUG_ON(event->wd!=wd);
			/*transfer system event to local type*/
			if (event->mask & (IN_MODIFY|IN_MOVE|IN_DELETE))
				ev|=fchg_write;
			if (event->mask & IN_ATTRIB)
				ev|=fchg_attrib;
			if (event->mask & IN_CREATE)
				ev|=fchg_extend;
			if (event->mask & IN_DELETE_SELF)
				ev|=fchg_delete;
			if (event->mask & IN_MOVE_SELF)
				ev|=fchg_rename;
		}
	}

	return ev;
}

int file_changed(int fd, int timeout)
{
	fd_set rfds;
	int n, nfd, wd, ev;
	char path[512], proc[512];
	struct timeval tv, *ptv = NULL;

	if (timeout > -1) {
		tv.tv_sec = timeout / 1000;
		tv.tv_usec = (timeout % 1000) * 1000;
		ptv = &tv;
	}

	/*文件描述符获取文件名*/
	snprintf(proc, sizeof(proc), "/proc/%d/fd/%d", get_process_id(), fd);
	n = readlink(proc, path, sizeof(path));
	if (skp_unlikely(n < 0))
		return fchg_failed;

	path[n] = '\0';

	/*创建*/
	nfd = inotify_init1(IN_NONBLOCK);
	if (skp_unlikely(nfd < 0))
		return fchg_failed;

	/*添加*/
	wd = inotify_add_watch(nfd, path,
			IN_MODIFY|IN_ATTRIB|IN_CREATE|IN_DELETE|IN_MOVE|IN_MOVE_SELF|
			IN_DELETE_SELF|IN_ONESHOT);
	if (skp_unlikely(wd < 0))
		goto fail;

	FD_ZERO(&rfds);
	FD_SET(nfd, &rfds);
	/*等待*/
	n = select(nfd + 1, &rfds, NULL, NULL, ptv);
	if (skp_unlikely(n < 0))
		goto fail;

	if (!n) {
		errno = ETIMEDOUT;
		goto fail;
	}

	BUG_ON(n!=1);
	/*分析结果*/
	ev = handle_inotify(nfd, wd);

	close(nfd);
	return ev;

fail:
	close(nfd);
	return fchg_failed;
}
#endif

int make_dir(const char *path, int mode)
{
	struct stat sb;
	char p_path[512], *d_name, *ptr, *pptr;
	int rc = snprintf(p_path, sizeof(p_path), "%s", path);
	if (rc > sizeof(p_path)-1)
		return -ENOMEM;

	/*移除末尾的路径分隔符*/
	while (p_path[rc-1]=='/')
		p_path[--rc] = '\0';

	d_name = strrchr(p_path, '/');
	if (!d_name)
		goto mkd;

	/*创建中间目录*/
	d_name+=1;
	pptr = p_path;
	while (1) {
		ptr = strchr(pptr, '/');
		BUG_ON(!ptr);
		if (ptr!=pptr) {
			ptr[0]='\0';
			rc = mkdir(p_path, mode);
			if (rc && errno!=EEXIST)
				return -errno;
			ptr[0]='/';
		}
		pptr=ptr+1;
		if (pptr==d_name)
			break;
	}

mkd:
	rc = stat(path, &sb);
	if (rc) {
		if (errno!=ENOENT)
			return -errno;
	} else {
		if ((sb.st_mode & S_IFMT) != S_IFDIR)
			return -ENOTDIR;
		return -EEXIST;
	}
	rc = mkdir(path, mode);
	if (rc)
		return -errno;
	return 0;
}
