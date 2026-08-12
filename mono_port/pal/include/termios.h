/* PAL <termios.h> — MiniOS Phase 0
 * metadata/console-unix.c 需要 struct termios 的完整定义（它在栈上放了一个
 * 局部变量，只有前向声明会报 "storage size of 'attr' isn't known"）。
 * MiniOS 的控制台是串口 + framebuffer，没有真正的 tty 层，
 * 所以 tc* 函数全部是良性桩。
 */
#ifndef PAL_TERMIOS_H
#define PAL_TERMIOS_H

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned char cc_t;
typedef unsigned int  speed_t;
typedef unsigned int  tcflag_t;

#define NCCS 32

struct termios {
    tcflag_t c_iflag;
    tcflag_t c_oflag;
    tcflag_t c_cflag;
    tcflag_t c_lflag;
    cc_t     c_line;
    cc_t     c_cc[NCCS];
    speed_t  c_ispeed;
    speed_t  c_ospeed;
};

/* c_cc 下标（Linux 取值） */
#define VINTR 0
#define VQUIT 1
#define VERASE 2
#define VKILL 3
#define VEOF  4
#define VTIME 5
#define VMIN  6
#define VSTART 8
#define VSTOP 9
#define VSUSP 10

/* c_iflag */
#define IGNBRK 0000001
#define BRKINT 0000002
#define ISTRIP 0000040
#define INLCR  0000100
#define IGNCR  0000200
#define ICRNL  0000400
#define IXON   0002000
#define IXOFF  0010000

/* c_oflag */
#define OPOST  0000001
#define ONLCR  0000004

/* c_lflag */
#define ISIG   0000001
#define ICANON 0000002
#define ECHO   0000010
#define ECHOE  0000020
#define ECHOK  0000040
#define ECHONL 0000100
#define NOFLSH 0000200
#define IEXTEN 0100000

/* tcsetattr 的 optional_actions */
#define TCSANOW   0
#define TCSADRAIN 1
#define TCSAFLUSH 2

/* tcflush() queue selectors -- console-unix.c does tcflush(fd, TCIFLUSH)
 * when it restores the terminal.  Values are the Linux ones. */
#define TCIFLUSH  0
#define TCOFLUSH  1
#define TCIOFLUSH 2

/* tcflow() actions */
#define TCOOFF    0
#define TCOON     1
#define TCIOFF    2
#define TCION     3

int  tcgetattr (int fd, struct termios *t);
int  tcsetattr (int fd, int optional_actions, const struct termios *t);
int  tcflush   (int fd, int queue_selector);
int  tcdrain   (int fd);
int  tcsendbreak (int fd, int duration);
speed_t cfgetispeed (const struct termios *t);
speed_t cfgetospeed (const struct termios *t);
int  cfsetispeed (struct termios *t, speed_t s);
int  cfsetospeed (struct termios *t, speed_t s);

#ifdef __cplusplus
}
#endif
#endif /* PAL_TERMIOS_H */
