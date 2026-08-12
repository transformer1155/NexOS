/* PAL <poll.h> — MiniOS Phase 0
 * 仅供 metadata/threadpool-io-poll.c 等文件取得 struct pollfd 形状。
 * Phase 0 没有网络/异步 IO，poll() 直接返回 -1/ENOSYS。
 */
#ifndef PAL_POLL_H
#define PAL_POLL_H

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned long nfds_t;

struct pollfd {
    int   fd;
    short events;
    short revents;
};

#define POLLIN     0x001
#define POLLPRI    0x002
#define POLLOUT    0x004
#define POLLERR    0x008
#define POLLHUP    0x010
#define POLLNVAL   0x020
#define POLLRDNORM 0x040
#define POLLRDBAND 0x080
#define POLLWRNORM 0x100
#define POLLWRBAND 0x200

int poll (struct pollfd *fds, nfds_t nfds, int timeout);

#ifdef __cplusplus
}
#endif
#endif /* PAL_POLL_H */
