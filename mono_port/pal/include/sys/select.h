/* Freestanding shim <sys/select.h> for the MiniOS Mono port (PAL).
 *
 * Only consumer so far is metadata/console-unix.c, which does a
 * `select(STDIN_FILENO+1, &rfds, NULL, NULL, &tv)` to implement
 * Console.KeyAvailable.  MiniOS has no readiness API in Phase 0, so the
 * implementation degrades to "stdin is always readable, everything else
 * never is" -- see pal/libc_posix.c.  That is the same answer a plain
 * blocking console gives, so Console.ReadKey() still behaves.
 *
 * Layout mirrors Linux/i386 (1024 bits, 32-bit words) so a host build of
 * the smoke tests and the MiniOS build agree on sizeof(fd_set).
 */
#ifndef PAL_SYS_SELECT_H
#define PAL_SYS_SELECT_H

#include <sys/types.h>
#include <sys/time.h>

#define FD_SETSIZE   1024
#define __NFDBITS    (8 * (int)sizeof (unsigned long))

typedef struct {
	unsigned long fds_bits[FD_SETSIZE / (8 * sizeof (unsigned long))];
} fd_set;

#define __FDELT(d)   ((d) / __NFDBITS)
#define __FDMASK(d)  ((unsigned long) 1 << ((d) % __NFDBITS))

#define FD_ZERO(s)   do {                                              \
		unsigned int __i;                                      \
		fd_set *__s = (s);                                     \
		for (__i = 0; __i < sizeof (fd_set) / sizeof (unsigned long); __i++) \
			__s->fds_bits[__i] = 0;                        \
	} while (0)

#define FD_SET(d, s)   ((void) ((s)->fds_bits[__FDELT (d)] |=  __FDMASK (d)))
#define FD_CLR(d, s)   ((void) ((s)->fds_bits[__FDELT (d)] &= ~__FDMASK (d)))
#define FD_ISSET(d, s) (((s)->fds_bits[__FDELT (d)] & __FDMASK (d)) != 0)

int select (int nfds, fd_set *readfds, fd_set *writefds,
            fd_set *exceptfds, struct timeval *timeout);

#endif /* PAL_SYS_SELECT_H */
