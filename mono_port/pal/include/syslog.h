/* Freestanding shim <syslog.h> for MiniOS Mono port (PAL).
 *
 * utils/mono-log-posix.c 是 Mono 日志的 syslog 后端。Mono 在运行时
 * 二选一：mono_log_open_syslog() 还是 mono_log_open_logfile()。
 * MiniOS 没有 syslogd，但有串口——所有内核调试都从那儿出来。
 * 所以这里把 syslog() 直接接到串口上：日志不丢，还能被
 * tools/test_*.py 的无头断言抓到，比丢弃有用得多。
 *
 * 实现在 pal/libc_posix.c。
 */
#ifndef PAL_SYSLOG_H
#define PAL_SYSLOG_H

#include <stdarg.h>

/* priorities */
#define LOG_EMERG   0
#define LOG_ALERT   1
#define LOG_CRIT    2
#define LOG_ERR     3
#define LOG_WARNING 4
#define LOG_NOTICE  5
#define LOG_INFO    6
#define LOG_DEBUG   7

/* openlog() options */
#define LOG_PID     0x01
#define LOG_CONS    0x02
#define LOG_ODELAY  0x04
#define LOG_NDELAY  0x08
#define LOG_NOWAIT  0x10
#define LOG_PERROR  0x20

/* facilities */
#define LOG_KERN    (0<<3)
#define LOG_USER    (1<<3)
#define LOG_DAEMON  (3<<3)
#define LOG_SYSLOG  (5<<3)
#define LOG_LOCAL0  (16<<3)

void openlog  (const char *ident, int option, int facility);
void syslog   (int priority, const char *format, ...);
void vsyslog  (int priority, const char *format, va_list ap);
void closelog (void);

#endif /* PAL_SYSLOG_H */
