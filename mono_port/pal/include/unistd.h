/* Freestanding shim <unistd.h> for MiniOS Mono port (PAL).
 *
 * Declarations only -- the implementations in pal/libc_impl.c are stubs for
 * Phase 0 and get wired to real MiniOS syscalls (SYS_READ/WRITE/OPEN/CLOSE
 * and friends) in Phase 1.
 */
#ifndef PAL_UNISTD_H
#define PAL_UNISTD_H

#include <stddef.h>
#include <stdio.h>      /* off_t / ssize_t / pid_t / SEEK_* live there */

/* Mono's mono-threads.h picks its backend off _POSIX_VERSION; without it the
 * header hits `#error "no backend support for current platform"`.  Claiming
 * POSIX.1-2008 steers it to USE_POSIX_BACKEND, which is the shape our
 * pthread shim implements. */
#define _POSIX_VERSION 200809L

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

/* access() mode bits */
#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4

ssize_t read  (int fd, void *buf, size_t count);
ssize_t write (int fd, const void *buf, size_t count);
int     close (int fd);
off_t   lseek (int fd, off_t offset, int whence);
int     unlink(const char *pathname);
int     access(const char *pathname, int mode);
char   *getcwd(char *buf, size_t size);
int     isatty(int fd);
pid_t   getpid(void);
uid_t   getuid(void);
unsigned int sleep(unsigned int seconds);
int     usleep(unsigned int usec);
int     getpagesize(void);
int     ftruncate(int fd, off_t length);
int     fsync (int fd);      /* FileStream.Flush(true) */
int     fdatasync(int fd);
int     dup  (int fd);
int     dup2 (int oldfd, int newfd);
int     pipe (int fds[2]);
int     rmdir(const char *path);
int     chdir(const char *path);
int     symlink(const char *target, const char *linkpath);
ssize_t readlink(const char *path, char *buf, size_t bufsiz);
uid_t   geteuid(void);
gid_t   getgid(void);
gid_t   getegid(void);   /* w32file-unix.c is_file_writable() */
pid_t   getppid(void);

/* sysconf() selectors Mono probes for. */
#define _SC_PAGESIZE          30
#define _SC_PAGE_SIZE         _SC_PAGESIZE
#define _SC_NPROCESSORS_ONLN  84
#define _SC_NPROCESSORS_CONF  83
long    sysconf(int name);

/* POSIX 里唯一由 libc 提供的全局变量。定义在 pal/libc_posix.c。
 * icall.c 遍历它来实现 Environment.GetEnvironmentVariables()。 */
extern char **environ;

#endif /* PAL_UNISTD_H */
