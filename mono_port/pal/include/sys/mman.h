/* Freestanding shim <sys/mman.h> for MiniOS Mono port (PAL).
 *
 * NOTE: config.h keeps HAVE_MMAP / HAVE_SYS_MMAN_H #undef on purpose, so
 * Mono takes its malloc-based fallback paths.  This header only exists so
 * that files which #include it unconditionally still compile; the
 * declarations below are backed by simple bump-heap emulation in
 * pal/libc_impl.c (mmap == aligned alloc, munmap == no-op).
 */
#ifndef PAL_SYS_MMAN_H
#define PAL_SYS_MMAN_H

#include <stddef.h>
#include <sys/types.h>

#define PROT_NONE  0x0
#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define PROT_EXEC  0x4

#define MAP_FILE      0x00
#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#define MAP_FIXED     0x10
#define MAP_ANON      0x20
#define MAP_ANONYMOUS MAP_ANON

#define MAP_FAILED ((void *) -1)

#define MS_ASYNC      1
#define MS_SYNC       4
#define MS_INVALIDATE 2

#define MREMAP_MAYMOVE 1
#define MREMAP_FIXED   2

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset);
void *mremap(void *old_address, size_t old_size, size_t new_size, int flags, ...);
int   munmap(void *addr, size_t length);
int   mprotect(void *addr, size_t len, int prot);
int   msync(void *addr, size_t length, int flags);
int   madvise(void *addr, size_t length, int advice);

#endif /* PAL_SYS_MMAN_H */
