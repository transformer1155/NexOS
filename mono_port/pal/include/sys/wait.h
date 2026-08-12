/* PAL <sys/wait.h> — MiniOS Phase 0
 * MiniOS 没有 fork/exec，w32process-unix.c 只是要 WIFEXITED/WIFSIGNALED
 * 这套宏来解释 wait status。宏按 Linux 的编码给出（低 7 位=信号，
 * 0x80 位=core dump，高 8 位=退出码），waitpid 本身返回 -1/ECHILD。
 */
#ifndef PAL_SYS_WAIT_H
#define PAL_SYS_WAIT_H

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WNOHANG    1
#define WUNTRACED  2
#define WCONTINUED 8

#define WEXITSTATUS(s)  (((s) & 0xFF00) >> 8)
#define WTERMSIG(s)     ((s) & 0x7F)
#define WSTOPSIG(s)     WEXITSTATUS(s)
#define WIFEXITED(s)    (WTERMSIG(s) == 0)
#define WIFSIGNALED(s)  (((signed char)(((s) & 0x7F) + 1) >> 1) > 0)
#define WIFSTOPPED(s)   (((s) & 0xFF) == 0x7F)
#define WIFCONTINUED(s) ((s) == 0xFFFF)
#define WCOREDUMP(s)    ((s) & 0x80)

pid_t wait    (int *status);
pid_t waitpid (pid_t pid, int *status, int options);

#ifdef __cplusplus
}
#endif
#endif /* PAL_SYS_WAIT_H */
