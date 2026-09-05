/* Freestanding shim <errno.h> for MiniOS Mono port (PAL).
 * Values follow Linux/x86 so that any errno we ever forward from the MiniOS
 * syscall layer (which already uses Linux numbering) matches.
 */
#ifndef PAL_ERRNO_H
#define PAL_ERRNO_H

extern int pal_errno;
#define errno pal_errno

#define EPERM            1
#define ENOENT           2
#define ESRCH            3
#define EINTR            4
#define EIO              5
#define ENXIO            6
#define E2BIG            7
#define ENOEXEC          8
#define EBADF            9
#define ECHILD          10
#define EAGAIN          11
#define ENOMEM          12
#define EACCES          13
#define EFAULT          14
#define EBUSY           16
#define EEXIST          17
#define EXDEV           18
#define ENODEV          19
#define ENOTDIR         20
#define EISDIR          21
#define EINVAL          22
#define ENFILE          23
#define EMFILE          24
#define ENOTTY          25
#define ETXTBSY         26
#define EFBIG           27
#define ENOSPC          28
#define ESPIPE          29
#define EROFS           30
#define EMLINK          31
#define EPIPE           32
#define EDOM            33
#define ERANGE          34
#define EDEADLK         35
#define ENAMETOOLONG    36
#define ENOLCK          37   /* w32file-unix.c 用它判断"锁不可用则忽略" */
#define ENOSYS          38
#define ENOTEMPTY       39
#define ELOOP           40
#define EWOULDBLOCK     EAGAIN
#define EOVERFLOW       75
#define EILSEQ          84
#define ENOTSUP         95
#define EOPNOTSUPP      95
#define ETIMEDOUT      110

#endif /* PAL_ERRNO_H */
