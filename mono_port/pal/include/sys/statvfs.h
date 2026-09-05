/* Freestanding shim <sys/statvfs.h> for the MiniOS Mono port (PAL).
 *
 * Backs DriveInfo.TotalSize / AvailableFreeSpace / TotalFreeSpace via
 * metadata/w32file-unix.c::mono_w32file_get_disk_free_space(), which reads
 * f_frsize, f_blocks, f_bfree and f_bavail.
 *
 * Layout is the Linux/i386 `struct statvfs`.
 */
#ifndef PAL_SYS_STATVFS_H
#define PAL_SYS_STATVFS_H

#include <sys/types.h>

typedef unsigned long fsblkcnt_t;
typedef unsigned long fsfilcnt_t;

struct statvfs {
	unsigned long f_bsize;    /* filesystem block size          */
	unsigned long f_frsize;   /* fragment size                  */
	fsblkcnt_t    f_blocks;   /* size of fs in f_frsize units   */
	fsblkcnt_t    f_bfree;    /* free blocks                    */
	fsblkcnt_t    f_bavail;   /* free blocks for unprivileged   */
	fsfilcnt_t    f_files;    /* inodes                         */
	fsfilcnt_t    f_ffree;    /* free inodes                    */
	fsfilcnt_t    f_favail;
	unsigned long f_fsid;
	unsigned long f_flag;
	unsigned long f_namemax;
	int           f_spare[6];
};

#define ST_RDONLY 1
#define ST_NOSUID 2

int statvfs  (const char *path, struct statvfs *buf);
int fstatvfs (int fd, struct statvfs *buf);

#endif /* PAL_SYS_STATVFS_H */
