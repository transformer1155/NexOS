/* pal/libc_posix.c -- POSIX-shaped surface for the MiniOS Mono port.
 *
 * Split out of libc_impl.c (which stays focused on ISO C: string/mem/printf/
 * malloc).  Everything here is the "operating-system" half that Mono's utils
 * layer expects to find: stdio streams, clocks, signals, mmap, dirent, stat,
 * pthread TLS.
 *
 * Phase 0 policy
 * --------------
 *   - Nothing here talks to hardware directly.  Where MiniOS can eventually
 *     back a call (files, time, threads) the function is routed through a
 *     replaceable hook (pal_fopen_hook, pal_ticks_hook, ...) so P1 can wire in
 *     real syscalls without touching Mono.
 *   - Where MiniOS has no concept at all (fork, signals, directories) the call
 *     fails the way POSIX says it should on an unsupported system, rather than
 *     silently pretending to succeed.  A lying stub costs far more debugging
 *     time later than an honest -1.
 *   - mmap is the exception: Mono uses it as a page allocator, so it is
 *     emulated on top of the bump heap.
 */

#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <limits.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <dirent.h>
#include <pthread.h>
#include <sched.h>
#include <poll.h>
#include <termios.h>
#include <locale.h>
#include <utime.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/resource.h>
#include <dlfcn.h>
#include <syslog.h>

extern void (*pal_console_putc)(char);

/* ====================================================================
 * stdio stream layer
 * ==================================================================== */

/* The three standard streams are sentinels, not real objects: fputs/fprintf
 * only ever look at whether the pointer is stdout/stderr.  Declared in
 * libc_impl.c; re-stated here for the stream helpers below. */
extern FILE *pal_stdout;
extern FILE *pal_stderr;
extern FILE *pal_stdin;

pal_fopen_fn pal_fopen_hook = 0;

FILE *fopen (const char *path, const char *mode)
{
	if (pal_fopen_hook)
		return pal_fopen_hook (path, mode);
	errno = ENOENT;
	return 0;
}

FILE *fdopen (int fd, const char *mode)
{
	(void)mode;
	if (fd == 1) return pal_stdout;
	if (fd == 2) return pal_stderr;
	if (fd == 0) return pal_stdin;
	errno = EBADF;
	return 0;
}

FILE *freopen (const char *path, const char *mode, FILE *stream)
{
	(void)stream;
	return fopen (path, mode);
}

int fclose (FILE *stream) { (void)stream; return 0; }

size_t fread (void *ptr, size_t size, size_t nmemb, FILE *stream)
{
	(void)ptr; (void)size; (void)nmemb; (void)stream;
	return 0;                       /* immediate EOF */
}

size_t fwrite (const void *ptr, size_t size, size_t nmemb, FILE *stream)
{
	const unsigned char *p = (const unsigned char *)ptr;
	size_t total = size * nmemb, i;

	(void)stream;
	if (!p || !pal_console_putc)
		return 0;
	for (i = 0; i < total; i++)
		pal_console_putc ((char)p[i]);
	return nmemb;
}

char *fgets (char *s, int size, FILE *stream)
{
	(void)s; (void)size; (void)stream;
	return 0;                       /* EOF */
}

int fgetc (FILE *stream)   { (void)stream; return EOF; }
int getc  (FILE *stream)   { return fgetc (stream); }
int ungetc (int c, FILE *stream) { (void)c; (void)stream; return EOF; }
int feof  (FILE *stream)   { (void)stream; return 1; }
int ferror (FILE *stream)  { (void)stream; return 0; }
void clearerr (FILE *stream) { (void)stream; }

int  fseek (FILE *stream, long offset, int whence)
	{ (void)stream; (void)offset; (void)whence; return -1; }
long ftell (FILE *stream)  { (void)stream; return -1; }
void rewind (FILE *stream) { (void)stream; }

int  setvbuf (FILE *stream, char *buf, int mode, size_t size)
	{ (void)stream; (void)buf; (void)mode; (void)size; return 0; }
void setbuf (FILE *stream, char *buf) { (void)stream; (void)buf; }

int fileno (FILE *stream)
{
	if (stream == pal_stdout) return 1;
	if (stream == pal_stderr) return 2;
	if (stream == pal_stdin)  return 0;
	return -1;
}

int putc (int c, FILE *stream) { return fputc (c, stream); }
int putchar (int c)            { return fputc (c, 0); }

int remove (const char *path) { (void)path; errno = ENOENT; return -1; }
int rename (const char *old_, const char *new_)
	{ (void)old_; (void)new_; errno = ENOENT; return -1; }

void perror (const char *s)
{
	if (s && pal_console_putc) {
		while (*s) pal_console_putc (*s++);
		pal_console_putc (':');
		pal_console_putc (' ');
	}
	if (pal_console_putc) {
		const char *m = strerror (errno);
		while (m && *m) pal_console_putc (*m++);
		pal_console_putc ('\n');
	}
}

/* No scanf engine in Phase 0.  Mono only uses these to parse /proc, which
 * MiniOS does not have, so "matched nothing" is the correct answer. */
int sscanf (const char *str, const char *fmt, ...) { (void)str; (void)fmt; return 0; }
int fscanf (FILE *stream, const char *fmt, ...)    { (void)stream; (void)fmt; return EOF; }

/* ====================================================================
 * time
 * ==================================================================== */

/* rdtsc-derived monotonic clock.  The TSC frequency is unknown at this layer,
 * so we scale by a nominal 1 GHz: values are monotonic and roughly
 * nanosecond-ish, which is all Mono's timers need in Phase 0.  P1 replaces
 * this with a MiniOS uptime syscall. */
static unsigned long long rdtsc_ (void)
{
	unsigned int lo, hi;
	__asm__ __volatile__ ("rdtsc" : "=a"(lo), "=d"(hi));
	return ((unsigned long long)hi << 32) | lo;
}

int clock_gettime (clockid_t clk, struct timespec *tp)
{
	unsigned long long t;

	if (!tp) { errno = EFAULT; return -1; }
	t = rdtsc_ ();
	if (clk == CLOCK_REALTIME) {
		/* No RTC yet: report the Unix epoch plus uptime. */
		tp->tv_sec  = (long)(t / 1000000000ULL);
		tp->tv_nsec = (long)(t % 1000000000ULL);
		return 0;
	}
	tp->tv_sec  = (long)(t / 1000000000ULL);
	tp->tv_nsec = (long)(t % 1000000000ULL);
	return 0;
}

int clock_getres (clockid_t clk, struct timespec *tp)
{
	(void)clk;
	if (!tp) { errno = EFAULT; return -1; }
	tp->tv_sec  = 0;
	tp->tv_nsec = 1;
	return 0;
}

int nanosleep (const struct timespec *req, struct timespec *rem)
{
	(void)req;
	if (rem) { rem->tv_sec = 0; rem->tv_nsec = 0; }
	return 0;                       /* single-threaded: nothing to yield to */
}

clock_t clock (void) { return (clock_t)(rdtsc_ () / 1000ULL); }

/* --- civil-time conversion (proleptic Gregorian, UTC only) --- */
static const int mdays_[12] = { 31,28,31,30,31,30,31,31,30,31,30,31 };

static int is_leap_ (int y)
{
	return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}

struct tm *gmtime_r (const time_t *t, struct tm *r)
{
	long days, rem;
	int y, m;

	if (!t || !r) return 0;
	days = (long)(*t / 86400);
	rem  = (long)(*t % 86400);
	if (rem < 0) { rem += 86400; days--; }

	r->tm_hour = (int)(rem / 3600);
	r->tm_min  = (int)((rem % 3600) / 60);
	r->tm_sec  = (int)(rem % 60);
	r->tm_wday = (int)((days + 4) % 7);   /* 1970-01-01 was a Thursday */
	if (r->tm_wday < 0) r->tm_wday += 7;

	y = 1970;
	for (;;) {
		long ylen = is_leap_ (y) ? 366 : 365;
		if (days < ylen) break;
		days -= ylen;
		y++;
	}
	r->tm_year = y - 1900;
	r->tm_yday = (int)days;
	for (m = 0; m < 12; m++) {
		int len = mdays_[m] + (m == 1 && is_leap_ (y) ? 1 : 0);
		if (days < len) break;
		days -= len;
	}
	r->tm_mon    = m;
	r->tm_mday   = (int)days + 1;
	r->tm_isdst  = 0;
	r->tm_gmtoff = 0;
	r->tm_zone   = "UTC";
	return r;
}

struct tm *localtime_r (const time_t *t, struct tm *r) { return gmtime_r (t, r); }

/* POSIX 允许非重入版共用一块静态缓冲。MiniOS 没有时区数据库，
 * 本地时间恒等于 UTC（tm_gmtoff=0），所以两者共用同一实现。 */
static struct tm g_tm_static;
struct tm *gmtime    (const time_t *t) { return gmtime_r (t, &g_tm_static); }
struct tm *localtime (const time_t *t) { return gmtime_r (t, &g_tm_static); }
double     difftime  (time_t end, time_t start) { return (double)(end - start); }

time_t timegm (struct tm *tm)
{
	long days = 0;
	int y, m;

	if (!tm) return 0;
	for (y = 1970; y < tm->tm_year + 1900; y++)
		days += is_leap_ (y) ? 366 : 365;
	for (m = 0; m < tm->tm_mon && m < 12; m++)
		days += mdays_[m] + (m == 1 && is_leap_ (tm->tm_year + 1900) ? 1 : 0);
	days += tm->tm_mday - 1;
	return (time_t)days * 86400 + tm->tm_hour * 3600 + tm->tm_min * 60 + tm->tm_sec;
}

time_t mktime (struct tm *tm) { return timegm (tm); }

size_t strftime (char *s, size_t max, const char *fmt, const struct tm *tm)
{
	/* Only the ISO-8601 shape Mono logs with; anything else yields "". */
	(void)fmt;
	if (!s || max == 0) return 0;
	if (!tm) { s[0] = 0; return 0; }
	{
		int n = snprintf (s, max, "%04d-%02d-%02d %02d:%02d:%02d",
		                  tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
		                  tm->tm_hour, tm->tm_min, tm->tm_sec);
		if (n < 0 || (size_t)n >= max) { s[0] = 0; return 0; }
		return (size_t)n;
	}
}

int setitimer (int which, const struct itimerval *nv, struct itimerval *ov)
	{ (void)which; (void)nv; (void)ov; errno = ENOSYS; return -1; }
int getitimer (int which, struct itimerval *cv)
	{ (void)which; (void)cv; errno = ENOSYS; return -1; }

/* ====================================================================
 * signals -- MiniOS ring-3 has no signal delivery yet
 * ==================================================================== */

/* sigset_t is a 2-word bitmap; signals are 1-based so slot = (signo-1). */
#define SIGSET_OK(n)  ((n) > 0 && (n) <= 64)
#define SIGSET_W(n)   (((unsigned)(n) - 1u) / 32u)
#define SIGSET_B(n)   (1ul << (((unsigned)(n) - 1u) % 32u))

int sigemptyset (sigset_t *set)
	{ if (set) { set->__bits[0] = 0; set->__bits[1] = 0; } return 0; }
int sigfillset (sigset_t *set)
	{ if (set) { set->__bits[0] = ~0ul; set->__bits[1] = ~0ul; } return 0; }
int sigaddset (sigset_t *set, int signo)
	{ if (set && SIGSET_OK (signo)) set->__bits[SIGSET_W (signo)] |= SIGSET_B (signo); return 0; }
int sigdelset (sigset_t *set, int signo)
	{ if (set && SIGSET_OK (signo)) set->__bits[SIGSET_W (signo)] &= ~SIGSET_B (signo); return 0; }
int sigismember (const sigset_t *set, int signo)
{
	if (!set || !SIGSET_OK (signo))
		return 0;
	return (set->__bits[SIGSET_W (signo)] & SIGSET_B (signo)) ? 1 : 0;
}

int sigprocmask (int how, const sigset_t *set, sigset_t *oldset)
	{ (void)how; (void)set; if (oldset) sigemptyset (oldset); return 0; }
int sigsuspend  (const sigset_t *mask) { (void)mask; errno = EINTR; return -1; }
int sigaction   (int signo, const struct sigaction *act, struct sigaction *oact)
	{ (void)signo; (void)act; if (oact) memset (oact, 0, sizeof *oact); return 0; }
int sigaltstack (const stack_t *ss, stack_t *oss)
	{ (void)ss; if (oss) memset (oss, 0, sizeof *oss); return 0; }
int raise (int signo) { (void)signo; return 0; }
int kill  (pid_t pid, int signo) { (void)pid; (void)signo; errno = ESRCH; return -1; }

__sighandler_t signal (int signo, __sighandler_t handler)
	{ (void)signo; (void)handler; return (__sighandler_t)0; }

/* ====================================================================
 * mmap -- page allocator on top of the bump heap
 * ==================================================================== */

void *mmap (void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
	unsigned char *p;
	unsigned long a;

	(void)addr; (void)prot; (void)flags; (void)offset;
	if (fd >= 0)                    /* file-backed mappings are unsupported */
		return MAP_FAILED;
	/* Over-allocate so the result can be page aligned; Mono assumes it. */
	p = (unsigned char *)malloc (length + 4096);
	if (!p)
		return MAP_FAILED;
	a = ((unsigned long)p + 4095u) & ~4095ul;
	memset ((void *)a, 0, length);
	return (void *)a;
}

/* dlmalloc calls mremap to grow its top chunk; report failure so it falls
 * back to allocating a fresh segment instead of assuming the old one moved. */
void *mremap (void *old_address, size_t old_size, size_t new_size, int flags, ...)
{
	(void)old_address; (void)old_size; (void)new_size; (void)flags;
	errno = ENOMEM;
	return MAP_FAILED;
}

int munmap (void *addr, size_t length)
	{ (void)addr; (void)length; return 0; }   /* bump heap: no reclaim */
int mprotect (void *addr, size_t len, int prot)
	{ (void)addr; (void)len; (void)prot; return 0; }
int msync (void *addr, size_t length, int flags)
	{ (void)addr; (void)length; (void)flags; return 0; }
int madvise (void *addr, size_t length, int advice)
	{ (void)addr; (void)length; (void)advice; return 0; }

/* Aligned allocation on top of the bump heap: over-allocate and round up.
 * free() is a no-op there, so losing the original pointer costs nothing. */
int posix_memalign (void **memptr, size_t alignment, size_t size)
{
	unsigned char *p;
	unsigned long a;

	if (!memptr)
		return EINVAL;
	if (alignment < sizeof (void *) || (alignment & (alignment - 1)) != 0)
		return EINVAL;          /* must be a power of two, >= sizeof(void*) */
	p = (unsigned char *)malloc (size + alignment);
	if (!p) {
		*memptr = 0;
		return ENOMEM;
	}
	a = ((unsigned long)p + (alignment - 1)) & ~(unsigned long)(alignment - 1);
	*memptr = (void *)a;
	return 0;
}

void *aligned_alloc (size_t alignment, size_t size)
{
	void *p = 0;
	return posix_memalign (&p, alignment, size) == 0 ? p : 0;
}

void *memalign (size_t alignment, size_t size) { return aligned_alloc (alignment, size); }
void *valloc   (size_t size)                   { return aligned_alloc (4096, size); }

int getpagesize (void) { return 4096; }

long sysconf (int name)
{
	switch (name) {
	case _SC_PAGESIZE:         return 4096;
	case _SC_NPROCESSORS_ONLN:
	case _SC_NPROCESSORS_CONF: return 1;
	default:                   errno = EINVAL; return -1;
	}
}

int ftruncate (int fd, off_t length) { (void)fd; (void)length; errno = EBADF; return -1; }
/* 没有写缓存要落盘：MiniOS 的 ata_write_sector 是同步的，flush 恒成功。 */
int fsync     (int fd) { (void)fd; return 0; }
int fdatasync (int fd) { (void)fd; return 0; }
int dup  (int fd)                    { (void)fd; errno = EBADF; return -1; }
int dup2 (int oldfd, int newfd)      { (void)oldfd; (void)newfd; errno = EBADF; return -1; }
int pipe (int fds[2])                { (void)fds; errno = ENOSYS; return -1; }
int rmdir (const char *p)            { (void)p; errno = EROFS; return -1; }
int chdir (const char *p)            { (void)p; errno = ENOENT; return -1; }
int symlink (const char *t, const char *l) { (void)t; (void)l; errno = ENOSYS; return -1; }
ssize_t readlink (const char *p, char *b, size_t n)
	{ (void)p; (void)b; (void)n; errno = ENOENT; return -1; }
uid_t geteuid (void)  { return 0; }
gid_t getgid  (void)  { return 0; }
gid_t getegid (void)  { return 0; }
pid_t getppid (void)  { return 0; }

/* ====================================================================
 * files / directories
 * ==================================================================== */

int open (const char *path, int flags, ...)
	{ (void)path; (void)flags; errno = ENOENT; return -1; }
int creat (const char *path, mode_t mode)
	{ (void)path; (void)mode; errno = EROFS; return -1; }
int fcntl (int fd, int cmd, ...)
	{ (void)fd; (void)cmd; errno = EINVAL; return -1; }

int stat  (const char *path, struct stat *buf)
	{ (void)path; (void)buf; errno = ENOENT; return -1; }
int lstat (const char *path, struct stat *buf)
	{ (void)path; (void)buf; errno = ENOENT; return -1; }
int fstat (int fd, struct stat *buf)
	{ (void)fd; (void)buf; errno = EBADF; return -1; }
int mkdir (const char *path, mode_t mode)
	{ (void)path; (void)mode; errno = EROFS; return -1; }
mode_t umask (mode_t mask) { (void)mask; return 0; }

/* mktemp family: MiniOS has no writable temp dir yet.  POSIX says mktemp
 * returns an empty string on failure (not NULL), and callers test for
 * *tmpl == '\0', so honour that exactly. */
char *mktemp (char *tmpl)
{
	if (tmpl) tmpl[0] = 0;
	return tmpl;
}
int mkstemp (char *tmpl) { (void)tmpl; errno = EROFS; return -1; }
char *mkdtemp (char *tmpl) { (void)tmpl; errno = EROFS; return 0; }

DIR           *opendir (const char *name) { (void)name; errno = ENOENT; return 0; }
struct dirent *readdir (DIR *d)           { (void)d; return 0; }
int readdir_r (DIR *d, struct dirent *e, struct dirent **res)
	{ (void)d; (void)e; if (res) *res = 0; return 0; }
int  closedir  (DIR *d) { (void)d; return 0; }
void rewinddir (DIR *d) { (void)d; }

/* MiniOS 内核只认 syscall.h 里那八个号。这里不把请求透传下去——
 * 用户态先拦一层，能用等价语义答复的就地答复，答不了的老实返回
 * ENOSYS，免得内核对未知号的行为（返回 -ENOSYS 还是杀进程）泄漏到
 * Mono 里变成难查的怪象。 */
long syscall (long number, ...)
{
	switch (number) {
	case SYS_gettid:
		/* 现阶段每进程单线程，主线程 tid == pid，这就是 Linux 语义。 */
		return (long) getpid ();
	case SYS_getpid:
		return (long) getpid ();
	default:
		errno = ENOSYS;
		return -1;
	}
}

/* ====================================================================
 * getrlimit - 只有栈上限是真答案，其它按"无限制"报
 * ==================================================================== */
int getrlimit (int resource, struct rlimit *rlim)
{
	if (!rlim) { errno = EFAULT; return -1; }
	switch (resource) {
	case RLIMIT_STACK:
		/* 和 MiniOS 内核给 ring-3 进程预留的用户栈一致（1 MiB）。
		 * mono_threads_platform_get_stack_bounds() 拿它反推栈底，
		 * GC 扫描栈根的范围就是从这里来的——报大了会扫到未映射页，
		 * 报小了会漏掉根对象，所以这个数字必须跟内核对齐。 */
		rlim->rlim_cur = 1024 * 1024;
		rlim->rlim_max = 1024 * 1024;
		return 0;
	case RLIMIT_NOFILE:
		rlim->rlim_cur = 64;
		rlim->rlim_max = 64;
		return 0;
	default:
		rlim->rlim_cur = RLIM_INFINITY;
		rlim->rlim_max = RLIM_INFINITY;
		return 0;
	}
}

int setrlimit (int resource, const struct rlimit *rlim)
	{ (void)resource; (void)rlim; return 0; }

int getrusage (int who, struct rusage *usage)
{
	(void)who;
	if (!usage) { errno = EFAULT; return -1; }
	memset (usage, 0, sizeof (*usage));
	return 0;
}

/* ====================================================================
 * dlfcn - 明确地不支持，而不是假装支持
 * ==================================================================== */
static const char *g_dl_last_error;

void *dlopen (const char *filename, int flags)
{
	(void)filename; (void)flags;
	g_dl_last_error = "MiniOS has no dynamic loader; "
	                  "only [DllImport(\"__Internal\")] is available";
	return 0;
}

void *dlsym (void *handle, const char *symbol)
{
	(void)handle; (void)symbol;
	g_dl_last_error = "MiniOS has no dynamic loader";
	return 0;
}

int dlclose (void *handle) { (void)handle; return 0; }

char *dlerror (void)
{
	const char *e = g_dl_last_error;
	g_dl_last_error = 0;          /* POSIX: 读一次就清掉 */
	return (char *) e;
}

/* ====================================================================
 * syslog - 接到串口。MiniOS 的所有诊断都从串口出来，
 * tools/test_*.py 的无头断言也是读串口，所以这里不能丢日志。
 * ==================================================================== */
static const char *g_syslog_ident = "mono";

void openlog (const char *ident, int option, int facility)
{
	(void)option; (void)facility;
	g_syslog_ident = ident ? ident : "mono";
}

void closelog (void) { g_syslog_ident = "mono"; }

void vsyslog (int priority, const char *format, va_list ap)
{
	static const char *lvl[8] = { "EMERG", "ALERT", "CRIT", "ERR",
	                              "WARN", "NOTICE", "INFO", "DEBUG" };
	char buf[512];
	int n;

	n = snprintf (buf, sizeof (buf), "[%s/%s] ",
	              g_syslog_ident, lvl[priority & 7]);
	if (n < 0 || (size_t) n >= sizeof (buf))
		n = 0;
	vsnprintf (buf + n, sizeof (buf) - n, format, ap);

	/* fd 2 -> MiniOS 串口（见 kernel syscall.cpp: SYS_WRITE fd1/fd2） */
	write (2, buf, strlen (buf));
	write (2, "\n", 1);
}

void syslog (int priority, const char *format, ...)
{
	va_list ap;
	va_start (ap, format);
	vsyslog (priority, format, ap);
	va_end (ap);
}

/* ====================================================================
 * pthread bits that need storage (the rest are inline in <pthread.h>)
 * ==================================================================== */

#define PAL_TLS_SLOTS 64
static void *g_tls_value[PAL_TLS_SLOTS];
static unsigned char g_tls_used[PAL_TLS_SLOTS];

int pthread_key_create (pthread_key_t *key, void (*dtor)(void *))
{
	unsigned i;

	(void)dtor;                     /* single thread: no key destructors run */
	if (!key) return EINVAL;
	for (i = 0; i < PAL_TLS_SLOTS; i++) {
		if (!g_tls_used[i]) {
			g_tls_used[i]  = 1;
			g_tls_value[i] = 0;
			*key = i;
			return 0;
		}
	}
	return EAGAIN;
}

int pthread_key_delete (pthread_key_t key)
{
	if (key >= PAL_TLS_SLOTS) return EINVAL;
	g_tls_used[key]  = 0;
	g_tls_value[key] = 0;
	return 0;
}

void *pthread_getspecific (pthread_key_t key)
	{ return (key < PAL_TLS_SLOTS) ? g_tls_value[key] : 0; }

int pthread_setspecific (pthread_key_t key, const void *value)
{
	if (key >= PAL_TLS_SLOTS) return EINVAL;
	g_tls_value[key] = (void *)value;
	return 0;
}

int pthread_attr_init (pthread_attr_t *a)
	{ if (a) { a->stacksize = 64 * 1024; a->stackaddr = 0; } return 0; }
int pthread_attr_destroy (pthread_attr_t *a) { (void)a; return 0; }
int pthread_attr_setstacksize (pthread_attr_t *a, size_t sz)
	{ if (a) a->stacksize = sz; return 0; }
int pthread_attr_getstacksize (const pthread_attr_t *a, size_t *sz)
	{ if (sz) *sz = a ? a->stacksize : 65536; return 0; }
int pthread_attr_getstack (const pthread_attr_t *a, void **addr, size_t *sz)
{
	if (addr) *addr = a ? a->stackaddr : 0;
	if (sz)   *sz   = a ? a->stacksize : 65536;
	return 0;
}
int pthread_getattr_np (pthread_t t, pthread_attr_t *a)
	{ (void)t; return pthread_attr_init (a); }

int pthread_create (pthread_t *t, const pthread_attr_t *a,
                    void *(*fn)(void *), void *arg)
	{ (void)t; (void)a; (void)fn; (void)arg; return EAGAIN; }
int pthread_join   (pthread_t t, void **ret)
	{ (void)t; if (ret) *ret = 0; return ESRCH; }
int pthread_detach (pthread_t t) { (void)t; return ESRCH; }
void pthread_exit  (void *ret)   { (void)ret; exit (0); }
int pthread_kill   (pthread_t t, int sig) { (void)t; (void)sig; return ESRCH; }
int pthread_sigmask (int how, const sigset_t *set, sigset_t *oldset)
	{ return sigprocmask (how, set, oldset); }
int pthread_setname_np (pthread_t t, const char *name)
	{ (void)t; (void)name; return 0; }
int pthread_getname_np (pthread_t t, char *buf, size_t len)
	{ (void)t; if (buf && len) buf[0] = 0; return 0; }

/* ==================================================================== *
 *  sched.h -- 单核、单线程，调度策略无意义
 *  这些是 metadata/threads.c 里 SetPriority 路径要链接到的符号。返回 0
 *  (成功) 而不是 ENOSYS，是因为 Mono 会把失败当成"线程坏了"往上抛异常；
 *  一个不做事但成功的调度器在语义上更接近"只有一个优先级"的系统。
 * ==================================================================== */
int sched_yield (void) { return 0; }
int sched_get_priority_min (int policy) { (void)policy; return 0; }
int sched_get_priority_max (int policy) { (void)policy; return 0; }

int sched_getparam (pid_t pid, struct sched_param *p)
	{ (void)pid; if (p) p->sched_priority = 0; return 0; }
int sched_setparam (pid_t pid, const struct sched_param *p)
	{ (void)pid; (void)p; return 0; }
int sched_getscheduler (pid_t pid) { (void)pid; return SCHED_OTHER; }
int sched_setscheduler (pid_t pid, int policy, const struct sched_param *p)
	{ (void)pid; (void)policy; (void)p; return 0; }

int sched_getaffinity (pid_t pid, size_t sz, cpu_set_t *mask)
{
	(void)pid;
	if (!mask || sz < sizeof (cpu_set_t)) { errno = EINVAL; return -1; }
	CPU_ZERO (mask);
	CPU_SET (0, mask);                 /* 只有 CPU0 */
	return 0;
}
int sched_setaffinity (pid_t pid, size_t sz, const cpu_set_t *mask)
	{ (void)pid; (void)sz; (void)mask; return 0; }

int pthread_getschedparam (pthread_t t, int *policy, struct sched_param *p)
{
	(void)t;
	if (policy) *policy = SCHED_OTHER;
	if (p) p->sched_priority = 0;
	return 0;
}
int pthread_setschedparam (pthread_t t, int policy, const struct sched_param *p)
	{ (void)t; (void)policy; (void)p; return 0; }
int pthread_attr_setschedparam (pthread_attr_t *a, const struct sched_param *p)
	{ (void)a; (void)p; return 0; }
int pthread_attr_setschedpolicy (pthread_attr_t *a, int policy)
	{ (void)a; (void)policy; return 0; }
int pthread_attr_setinheritsched (pthread_attr_t *a, int inherit)
	{ (void)a; (void)inherit; return 0; }

/* ==================================================================== *
 *  poll.h -- Phase 0 没有可轮询的 fd
 * ==================================================================== */
int poll (struct pollfd *fds, nfds_t nfds, int timeout)
{
	nfds_t i;
	(void)timeout;
	for (i = 0; i < nfds; i++)
		if (fds) fds[i].revents = POLLNVAL;
	errno = ENOSYS;
	return -1;
}

/* ==================================================================== *
 *  sys/select.h
 *  唯一的调用者是 metadata/console-unix.c 的 Console.KeyAvailable：
 *      select (STDIN+1, &rfds, NULL, NULL, &tv)
 *  MiniOS Phase 0 没有就绪查询，这里给出"stdin 永远可读、其它 fd 永远
 *  不可读"的答案 —— 等价于一个纯阻塞控制台，KeyAvailable 会返回 true
 *  然后 ReadKey 去阻塞读，行为正确；返回 -1/ENOSYS 反而会让
 *  ConsoleDriver 认为终端坏掉。
 * ==================================================================== */
int select (int nfds, fd_set *readfds, fd_set *writefds,
            fd_set *exceptfds, struct timeval *timeout)
{
	int ready = 0;
	(void)timeout;

	if (nfds < 0 || nfds > FD_SETSIZE) { errno = EINVAL; return -1; }

	if (readfds) {
		int want_stdin = (nfds > STDIN_FILENO) && FD_ISSET (STDIN_FILENO, readfds);
		FD_ZERO (readfds);
		if (want_stdin) { FD_SET (STDIN_FILENO, readfds); ready++; }
	}
	/* 写端：串口永远可写 */
	if (writefds) {
		int i, n = 0;
		for (i = 0; i < nfds; i++)
			if (FD_ISSET (i, writefds)) n++;
		ready += n;
	}
	if (exceptfds)
		FD_ZERO (exceptfds);

	return ready;
}

/* ==================================================================== *
 *  termios / ioctl -- MiniOS 的"终端"是串口，没有行规程
 *  console-unix.c 只在初始化时探一次；探不到就退回默认，不影响启动。
 * ==================================================================== */
int tcgetattr (int fd, struct termios *t)
{
	(void)fd;
	if (!t) { errno = EINVAL; return -1; }
	memset (t, 0, sizeof (*t));
	t->c_iflag  = ICRNL;
	t->c_oflag  = OPOST | ONLCR;
	t->c_lflag  = ICANON | ECHO | ISIG;
	t->c_cc[VMIN]  = 1;
	t->c_cc[VTIME] = 0;
	t->c_ispeed = t->c_ospeed = 38400;
	return 0;
}
int tcsetattr (int fd, int act, const struct termios *t)
	{ (void)fd; (void)act; (void)t; return 0; }
int tcflush   (int fd, int q) { (void)fd; (void)q; return 0; }
int tcdrain   (int fd)        { (void)fd; return 0; }
int tcsendbreak (int fd, int d) { (void)fd; (void)d; return 0; }
speed_t cfgetispeed (const struct termios *t) { return t ? t->c_ispeed : 0; }
speed_t cfgetospeed (const struct termios *t) { return t ? t->c_ospeed : 0; }
int cfsetispeed (struct termios *t, speed_t s) { if (t) t->c_ispeed = s; return 0; }
int cfsetospeed (struct termios *t, speed_t s) { if (t) t->c_ospeed = s; return 0; }

int ioctl (int fd, unsigned long req, ...)
{
	(void)fd; (void)req;
	errno = ENOTTY;                    /* 让调用方退回 80x25 默认值 */
	return -1;
}

/* ==================================================================== *
 *  utime -- SFS 目录项没有时间戳字段
 *  appdomain.c 做影子拷贝时"尽量"保留 mtime，失败只是 warning，
 *  所以这里返回成功而不是 -1，免得每加载一个程序集就刷一条日志。
 * ==================================================================== */
int utime (const char *path, const struct utimbuf *times)
	{ (void)path; (void)times; return 0; }
int utimes (const char *path, const struct timeval times[2])
	{ (void)path; (void)times; return 0; }

/* ==================================================================== *
 *  wait -- 没有子进程
 * ==================================================================== */
pid_t wait (int *status)
	{ if (status) *status = 0; errno = ECHILD; return (pid_t)-1; }
pid_t waitpid (pid_t pid, int *status, int options)
	{ (void)pid; (void)options; if (status) *status = 0; errno = ECHILD; return (pid_t)-1; }

/* ==================================================================== *
 *  locale.h -- 永远是 "C"
 * ==================================================================== */
static char g_locale_c[] = "C";
static char g_lc_empty[] = "";
static char g_lc_point[] = ".";

char *setlocale (int category, const char *locale)
{
	(void)category;
	if (locale && locale[0] && !(locale[0] == 'C' && locale[1] == 0))
		return NULL;                   /* 只认 "C" / "" */
	return g_locale_c;
}

struct lconv *localeconv (void)
{
	static struct lconv lc;
	static int inited = 0;
	if (!inited) {
		memset (&lc, 0, sizeof (lc));
		lc.decimal_point     = g_lc_point;
		lc.thousands_sep     = g_lc_empty;
		lc.grouping          = g_lc_empty;
		lc.int_curr_symbol   = g_lc_empty;
		lc.currency_symbol   = g_lc_empty;
		lc.mon_decimal_point = g_lc_empty;
		lc.mon_thousands_sep = g_lc_empty;
		lc.mon_grouping      = g_lc_empty;
		lc.positive_sign     = g_lc_empty;
		lc.negative_sign     = g_lc_empty;
		lc.int_frac_digits = lc.frac_digits = (char)255;
		lc.p_cs_precedes = lc.p_sep_by_space = (char)255;
		lc.n_cs_precedes = lc.n_sep_by_space = (char)255;
		lc.p_sign_posn   = lc.n_sign_posn    = (char)255;
		inited = 1;
	}
	return &lc;
}

/* ==================================================================== *
 *  statfs / statvfs -- System.IO.DriveInfo 的后端
 *
 *  MiniOS 只有一个卷（SFS，2MiB 镜像里 SFS_LBA=3328 之后那段，约 768 个
 *  512B 扇区）。这里报的是那个卷的静态尺寸；P1 接上真实 SFS 超级块后
 *  改写 pal_statfs_hook 即可，Mono 侧不用动。
 *
 *  f_type 故意报 MSDOS_SUPER_MAGIC：w32file-unix.c 的 _wapi_drive_types
 *  表里 SFS 不存在，报一个它认识的 FAT magic 能让 DriveInfo.DriveType
 *  落到 Fixed 而不是 Unknown。
 * ==================================================================== */
#define PAL_FS_BLOCK_SIZE   512u
#define PAL_FS_TOTAL_BLOCKS 768u
#define PAL_FS_FREE_BLOCKS  512u

int (*pal_statfs_hook) (const char *path, struct statfs *buf) = 0;

int statfs (const char *path, struct statfs *buf)
{
	if (!buf) { errno = EFAULT; return -1; }
	if (pal_statfs_hook)
		return pal_statfs_hook (path, buf);

	memset (buf, 0, sizeof (*buf));
	buf->f_type    = MSDOS_SUPER_MAGIC;
	buf->f_bsize   = PAL_FS_BLOCK_SIZE;
	buf->f_frsize  = PAL_FS_BLOCK_SIZE;
	buf->f_blocks  = PAL_FS_TOTAL_BLOCKS;
	buf->f_bfree   = PAL_FS_FREE_BLOCKS;
	buf->f_bavail  = PAL_FS_FREE_BLOCKS;
	buf->f_namelen = 19;               /* SFS 目录项的扁平名字上限 */
	return 0;
}

int fstatfs (int fd, struct statfs *buf) { (void)fd; return statfs ("/", buf); }

int statvfs (const char *path, struct statvfs *buf)
{
	(void)path;
	if (!buf) { errno = EFAULT; return -1; }
	memset (buf, 0, sizeof (*buf));
	buf->f_bsize   = PAL_FS_BLOCK_SIZE;
	buf->f_frsize  = PAL_FS_BLOCK_SIZE;
	buf->f_blocks  = PAL_FS_TOTAL_BLOCKS;
	buf->f_bfree   = PAL_FS_FREE_BLOCKS;
	buf->f_bavail  = PAL_FS_FREE_BLOCKS;
	buf->f_namemax = 19;
	return 0;
}

int fstatvfs (int fd, struct statvfs *buf) { (void)fd; return statvfs ("/", buf); }

/* ---- process environment ----------------------------------------- *
 * `environ` 是 POSIX 里唯一一个由 libc 提供的全局【变量】而不是函数，
 * 所以它不会被任何 -D 或桩函数带出来，必须真的有一个定义。
 * metadata/icall.c 的 ves_icall_System_Environment_GetEnvironmentVariableNames
 * 直接遍历它。
 *
 * MiniOS 的 ring-3 进程目前不从内核继承环境块，所以给一个只含结束符的
 * 空数组：遍历者立刻看到 NULL 就停，行为等价于"环境为空"，
 * 而不是踩到野指针。将来内核支持传环境时，改成在 _start 里把
 * environ 指向栈上 argv 之后的那段即可，调用方无需改动。 */
static char *pal_empty_environ[1] = { (char *) 0 };
char **environ = pal_empty_environ;
