/* Freestanding shim <sys/resource.h> for MiniOS Mono port (PAL).
 *
 * 唯一的真实客户是 utils/mono-threads-posix.c:156 的
 * mono_threads_platform_get_stack_bounds() 兜底分支：
 *
 *     struct rlimit lim;
 *     if (getrlimit (RLIMIT_STACK, &lim) == 0) ... 
 *
 * 它拿栈上限来推算当前线程的栈范围，GC 扫描栈根要用。给一个真实的
 * 常量比返回 -1 好：返回 -1 会让 Mono 退化到 "stack bounds unknown"，
 * 保守扫描整段地址空间。
 *
 * MiniOS 的 ring-3 进程栈由内核在 USER_END 之下布置，见 kernel 的
 * USER_BASE=0x04000000 / USER_END=0x08000000。这里报 1 MiB，
 * 和内核给用户栈预留的一致。
 */
#ifndef PAL_SYS_RESOURCE_H
#define PAL_SYS_RESOURCE_H

#include <sys/types.h>

typedef unsigned long rlim_t;

struct rlimit {
	rlim_t rlim_cur;   /* soft limit */
	rlim_t rlim_max;   /* hard limit */
};

/* Linux i386 的编号，照抄以免哪天真接上内核时对不上。 */
#define RLIMIT_CPU     0
#define RLIMIT_FSIZE   1
#define RLIMIT_DATA    2
#define RLIMIT_STACK   3
#define RLIMIT_CORE    4
#define RLIMIT_NOFILE  7
#define RLIMIT_AS      9

#define RLIM_INFINITY  ((rlim_t) -1)

int getrlimit (int resource, struct rlimit *rlim);
int setrlimit (int resource, const struct rlimit *rlim);

/* getrusage 只为让偶尔无条件引用它的文件能过编译。config.h 里
 * HAVE_GETRUSAGE 是 #undef 的，所以 Mono 不会走这条路。 */
#define RUSAGE_SELF      0
#define RUSAGE_CHILDREN (-1)

struct rusage {
	struct { long tv_sec; long tv_usec; } ru_utime;
	struct { long tv_sec; long tv_usec; } ru_stime;
	long ru_maxrss;
	long ru_minflt;
	long ru_majflt;
	long ru_nvcsw;
	long ru_nivcsw;
};

int getrusage (int who, struct rusage *usage);

#endif /* PAL_SYS_RESOURCE_H */
