/* PAL <sched.h> — MiniOS Phase 0
 *
 * MiniOS 目前是单线程 ring-3，调度策略没有意义；这里只提供
 * metadata/threads.c 与 utils/mono-threads-posix.c 编译所需的形状：
 *   struct sched_param / SCHED_* / sched_yield / 亲和性集合。
 * 所有函数在 Phase 0 都是良性桩（成功但不做事，或返回 ENOSYS）。
 */
#ifndef PAL_SCHED_H
#define PAL_SCHED_H

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

struct sched_param {
    int sched_priority;
};

#define SCHED_OTHER   0
#define SCHED_FIFO    1
#define SCHED_RR      2
#define SCHED_BATCH   3
#define SCHED_IDLE    5

int sched_yield (void);
int sched_get_priority_min (int policy);
int sched_get_priority_max (int policy);
int sched_getparam (pid_t pid, struct sched_param *param);
int sched_setparam (pid_t pid, const struct sched_param *param);
int sched_getscheduler (pid_t pid);
int sched_setscheduler (pid_t pid, int policy, const struct sched_param *param);

/* ---- CPU 亲和性：单核，固定为 CPU0 ---------------------------------- */
#define CPU_SETSIZE 32
typedef struct { unsigned long __bits[1]; } cpu_set_t;

#define CPU_ZERO(s)      ((s)->__bits[0] = 0UL)
#define CPU_SET(c, s)    ((s)->__bits[0] |= (1UL << ((c) & 31)))
#define CPU_CLR(c, s)    ((s)->__bits[0] &= ~(1UL << ((c) & 31)))
#define CPU_ISSET(c, s)  (((s)->__bits[0] >> ((c) & 31)) & 1UL)
#define CPU_COUNT(s)     (__builtin_popcountl ((s)->__bits[0]))

int sched_getaffinity (pid_t pid, size_t cpusetsize, cpu_set_t *mask);
int sched_setaffinity (pid_t pid, size_t cpusetsize, const cpu_set_t *mask);

#ifdef __cplusplus
}
#endif
#endif /* PAL_SCHED_H */
