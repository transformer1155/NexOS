/* Freestanding shim <sys/time.h> for MiniOS Mono port (PAL).
 * struct timeval / struct timespec live in <time.h>; this header just
 * re-exports them plus the BSD-era helpers Mono still uses.
 */
#ifndef PAL_SYS_TIME_H
#define PAL_SYS_TIME_H

#include <sys/types.h>
#include <time.h>

struct itimerval {
    struct timeval it_interval;
    struct timeval it_value;
};

#define ITIMER_REAL    0
#define ITIMER_VIRTUAL 1
#define ITIMER_PROF    2

#define timerisset(tvp)     ((tvp)->tv_sec || (tvp)->tv_usec)
#define timerclear(tvp)     ((tvp)->tv_sec = (tvp)->tv_usec = 0)
#define timercmp(a, b, CMP) \
    (((a)->tv_sec == (b)->tv_sec) ? ((a)->tv_usec CMP (b)->tv_usec) \
                                  : ((a)->tv_sec  CMP (b)->tv_sec))

int gettimeofday (struct timeval *tv, void *tz);
int setitimer    (int which, const struct itimerval *nv, struct itimerval *ov);
int getitimer    (int which, struct itimerval *cv);

#endif /* PAL_SYS_TIME_H */
