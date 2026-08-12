/* PAL <utime.h> — MiniOS Phase 0
 * appdomain.c 做程序集影子拷贝时会把源文件的时间戳搬到副本上。
 * MiniOS 的 SFS 目录项没有时间戳字段，所以 utime() 是良性 no-op：
 * 返回 0（成功），因为 Mono 只是"尽量保留"时间戳，失败会打 warning。
 */
#ifndef PAL_UTIME_H
#define PAL_UTIME_H

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

struct utimbuf {
    time_t actime;   /* 访问时间 */
    time_t modtime;  /* 修改时间 */
};

int utime  (const char *path, const struct utimbuf *times);
int utimes (const char *path, const struct timeval times[2]);

#ifdef __cplusplus
}
#endif
#endif /* PAL_UTIME_H */
