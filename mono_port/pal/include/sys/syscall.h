/* Freestanding shim <sys/syscall.h> for MiniOS Mono port (PAL).
 *
 * MiniOS uses its own int 0x80 numbering (see kernel syscall.h):
 *   SYS_EXIT=1 SYS_READ=3 SYS_WRITE=4 SYS_OPEN=5 SYS_CLOSE=6
 *   SYS_GETPID=20 SYS_GETUID=24 SYS_BRK=45
 * Only the few __NR_* names Mono probes for are declared here; anything
 * Mono needs that MiniOS lacks stays undefined so the #ifdef takes the
 * portable fallback path.
 */
#ifndef PAL_SYS_SYSCALL_H
#define PAL_SYS_SYSCALL_H

#define __NR_exit    1
#define __NR_read    3
#define __NR_write   4
#define __NR_open    5
#define __NR_close   6
#define __NR_getpid  20
#define __NR_getuid  24
#define __NR_brk     45

/* Linux i386 的 gettid。MiniOS 内核【没有】实现这个号，但
 * utils/mono-threads-linux.c 的 mono_native_thread_os_id_get() 无条件
 * 用 syscall(SYS_gettid)，不定义就直接编译失败。
 * pal/libc_posix.c 的 syscall() 在用户态把它拦下来返回 getpid()——
 * 在"每进程一个线程"的当前阶段这正是 Linux 的语义（主线程 tid == pid），
 * 而不是一个谎。等 P1 上了真线程再改成查线程控制块。 */
#define __NR_gettid  224

#define SYS_exit   __NR_exit
#define SYS_read   __NR_read
#define SYS_write  __NR_write
#define SYS_getpid __NR_getpid
#define SYS_gettid __NR_gettid

long syscall(long number, ...);

#endif /* PAL_SYS_SYSCALL_H */
