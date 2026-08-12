/* PAL <sys/ioctl.h> — MiniOS Phase 0
 * console-unix.c 用 TIOCGWINSZ 问终端尺寸。MiniOS 没有 tty ioctl，
 * ioctl() 一律 -1/ENOTTY，调用方会退回默认 80x25。
 */
#ifndef PAL_SYS_IOCTL_H
#define PAL_SYS_IOCTL_H

#ifdef __cplusplus
extern "C" {
#endif

struct winsize {
    unsigned short ws_row;
    unsigned short ws_col;
    unsigned short ws_xpixel;
    unsigned short ws_ypixel;
};

#define TIOCGWINSZ 0x5413
#define TIOCSWINSZ 0x5414
#define FIONREAD   0x541B
#define FIONBIO    0x5421

int ioctl (int fd, unsigned long request, ...);

#ifdef __cplusplus
}
#endif
#endif /* PAL_SYS_IOCTL_H */
