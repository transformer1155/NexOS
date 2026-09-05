/* Freestanding shim <time.h> for MiniOS Mono port (PAL). */
#ifndef PAL_TIME_H
#define PAL_TIME_H
#include <stddef.h>

typedef long time_t;
typedef int  clockid_t;

#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1
#define TIMER_ABSTIME   1

struct timeval {
    long tv_sec;
    long tv_usec;
};

struct timespec {
    long tv_sec;
    long tv_nsec;
};

#ifndef PAL_HAVE_TIMEZONE_STRUCT
#define PAL_HAVE_TIMEZONE_STRUCT 1
struct timezone {
    int tz_minuteswest;
    int tz_dsttime;
};
#endif

struct tm {
    int tm_sec;
    int tm_min;
    int tm_hour;
    int tm_mday;
    int tm_mon;
    int tm_year;
    int tm_wday;
    int tm_yday;
    int tm_isdst;
    long tm_gmtoff;
    const char *tm_zone;
};

typedef long clock_t;
#define CLOCKS_PER_SEC 1000000L

time_t time(time_t *t);
int    gettimeofday(struct timeval *tv, void *tz);
clock_t clock(void);

/* Monotonic source is the TSC; realtime is a fixed epoch until MiniOS
 * grows an RTC syscall (P1). */
int clock_gettime(clockid_t clk, struct timespec *tp);
int clock_getres (clockid_t clk, struct timespec *tp);
int nanosleep(const struct timespec *req, struct timespec *rem);

struct tm *gmtime_r   (const time_t *t, struct tm *result);
struct tm *localtime_r(const time_t *t, struct tm *result);
/* Non-reentrant forms return a pointer into one shared static buffer, as
 * POSIX allows.  metadata/threads.c (mono_local_time) uses localtime(). */
struct tm *gmtime     (const time_t *t);
struct tm *localtime  (const time_t *t);
double     difftime   (time_t end, time_t start);
time_t     mktime(struct tm *tm);
time_t     timegm(struct tm *tm);
size_t     strftime(char *s, size_t max, const char *fmt, const struct tm *tm);

#endif /* PAL_TIME_H */
