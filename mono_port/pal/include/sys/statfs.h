/* Freestanding shim <sys/statfs.h> for the MiniOS Mono port (PAL).
 *
 * metadata/w32file-unix.c uses statfs() for two things:
 *   GetDriveTypeFromPath()  -> buf.f_type, mapped through _wapi_drive_types
 *   get_fstypename()        -> buf.f_type, same table, gives the string
 * i.e. DriveInfo.DriveType / DriveInfo.DriveFormat.
 *
 * Layout is the Linux/i386 `struct statfs` so the field offsets match what a
 * host build sees.  MiniOS answers with its SFS identity -- see
 * pal/libc_posix.c; f_type is reported as MSDOS_SUPER_MAGIC, which makes
 * Mono classify the volume as a plain fixed disk instead of "unknown".
 */
#ifndef PAL_SYS_STATFS_H
#define PAL_SYS_STATFS_H

#include <sys/types.h>

typedef struct { int __val[2]; } __pal_fsid_t;

struct statfs {
	long          f_type;     /* filesystem magic (see below)   */
	long          f_bsize;    /* optimal transfer block size    */
	unsigned long f_blocks;   /* total data blocks              */
	unsigned long f_bfree;    /* free blocks                    */
	unsigned long f_bavail;   /* free blocks for unprivileged   */
	unsigned long f_files;    /* total inodes                   */
	unsigned long f_ffree;    /* free inodes                    */
	__pal_fsid_t  f_fsid;
	long          f_namelen;  /* max filename length            */
	long          f_frsize;   /* fragment size                  */
	long          f_flags;
	long          f_spare[4];
};

/* A couple of the magics from Linux's magic.h that Mono's drive-type table
 * looks for.  MiniOS/SFS is closest to a plain FAT volume. */
#define MSDOS_SUPER_MAGIC   0x4d44
#define EXT2_SUPER_MAGIC    0xEF53
#define TMPFS_MAGIC         0x01021994

int statfs  (const char *path, struct statfs *buf);
int fstatfs (int fd, struct statfs *buf);

#endif /* PAL_SYS_STATFS_H */
