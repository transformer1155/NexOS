/* Freestanding shim <fcntl.h> for MiniOS Mono port (PAL).
 *
 * Flag values mirror Linux/i386 so the numbers we hand to the MiniOS VFS
 * syscalls (and anything Mono hard-codes) agree with the host build used for
 * smoke testing.
 */
#ifndef PAL_FCNTL_H
#define PAL_FCNTL_H

#include <sys/types.h>

#define O_RDONLY    0x0000
#define O_WRONLY    0x0001
#define O_RDWR      0x0002
#define O_ACCMODE   0x0003
#define O_CREAT     0x0040
#define O_EXCL      0x0080
#define O_NOCTTY    0x0100
#define O_TRUNC     0x0200
#define O_APPEND    0x0400
#define O_NONBLOCK  0x0800
#define O_SYNC      0x1000
#define O_LARGEFILE 0x8000
#define O_DIRECTORY 0x10000
#define O_CLOEXEC   0x80000
#define O_BINARY    0            /* POSIX has no text/binary split */

/* fcntl() commands */
#define F_DUPFD     0
#define F_GETFD     1
#define F_SETFD     2
#define F_GETFL     3
#define F_SETFL     4
#define F_GETLK     5
#define F_SETLK     6
#define F_SETLKW    7
#define FD_CLOEXEC  1

/* Advisory record locks.  metadata/w32file-unix.c implements
 * LockFile/UnlockFile with fcntl(F_SETLK) on a struct flock; the layout and
 * the l_type values are the Linux/i386 ones. */
#define F_RDLCK     0
#define F_WRLCK     1
#define F_UNLCK     2

struct flock {
	short l_type;    /* F_RDLCK / F_WRLCK / F_UNLCK   */
	short l_whence;  /* SEEK_SET / SEEK_CUR / SEEK_END */
	off_t l_start;
	off_t l_len;
	pid_t l_pid;
};

int open  (const char *path, int flags, ...);
int creat (const char *path, mode_t mode);
int fcntl (int fd, int cmd, ...);

#endif /* PAL_FCNTL_H */
