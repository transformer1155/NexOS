/* PAL <linux/fs.h> — MiniOS Phase 0
 * w32file-unix.c 用 BLKGETSIZE64 问块设备容量（DriveInfo.TotalSize）。
 * MiniOS 的 ioctl() 一律 ENOTTY，所以这里只需要常量存在即可编译；
 * 运行期取容量会失败，DriveInfo 回落到 0。
 */
#ifndef PAL_LINUX_FS_H
#define PAL_LINUX_FS_H

#define BLKGETSIZE64 0x80081272UL
#define BLKGETSIZE   0x00001260UL
#define BLKSSZGET    0x00001268UL
#define BLKFLSBUF    0x00001261UL

#endif /* PAL_LINUX_FS_H */
