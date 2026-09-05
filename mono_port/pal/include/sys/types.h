/* Freestanding shim <sys/types.h> for MiniOS Mono port (PAL).
 *
 * MiniOS ring-3 is 32-bit ILP32: long == int == pointer == 4 bytes.
 * The core POSIX scalar typedefs live behind the SAME guard macro that
 * pal/include/stdio.h uses (PAL_HAVE_POSIX_TYPES), so whichever header is
 * included first wins and the other one silently skips -- no redefinitions.
 */
#ifndef PAL_SYS_TYPES_H
#define PAL_SYS_TYPES_H

#include <stddef.h>
#include <stdint.h>

#ifndef PAL_HAVE_POSIX_TYPES
#define PAL_HAVE_POSIX_TYPES 1
typedef long          off_t;
typedef int           ssize_t;
typedef int           pid_t;
typedef unsigned int  mode_t;
typedef unsigned int  uid_t;
typedef unsigned int  gid_t;
#endif

/* Filesystem scalars. POSIX puts these in <sys/types.h>, and <dirent.h>
 * needs ino_t without pulling in all of <sys/stat.h>, so they live here and
 * sys/stat.h just includes us. */
#ifndef PAL_HAVE_FS_TYPES
#define PAL_HAVE_FS_TYPES 1
typedef unsigned int  dev_t;
typedef unsigned int  ino_t;
typedef unsigned int  nlink_t;
typedef long          blksize_t;
typedef long          blkcnt_t;
#endif

/* Types that only <sys/types.h> is expected to provide. */
typedef long           off64_t;
typedef unsigned int   useconds_t;
typedef int            suseconds_t;
typedef unsigned int   socklen_t;
typedef unsigned int   in_addr_t;
typedef unsigned short in_port_t;
typedef unsigned int   id_t;
typedef int            key_t;

typedef unsigned char  u_char;
typedef unsigned short u_short;
typedef unsigned int   u_int;
typedef unsigned long  u_long;

#endif /* PAL_SYS_TYPES_H */
