/* Freestanding shim <signal.h> for MiniOS Mono port (PAL).
 *
 * Phase 0 has no signal delivery at all: the interpreter runs single-threaded
 * in ring 3 and does not use SIGSEGV-based null checks or SIGUSR-based
 * suspend.  Everything here exists so Mono's headers *type-check*; the entry
 * points are stubs that report success without doing anything.
 *
 * Layout note: `ucontext_t` deliberately mirrors Linux/i386 (gregs[19] with
 * the REG_* indices below) because mono/utils/mono-context.h indexes
 * uc_mcontext.gregs[] positionally on x86.  Do not reorder.
 */
#ifndef PAL_SIGNAL_H
#define PAL_SIGNAL_H

#include <stddef.h>
#include <sys/types.h>

/* ---- signal numbers (Linux/i386) ---- */
#define SIGHUP     1
#define SIGINT     2
#define SIGQUIT    3
#define SIGILL     4
#define SIGTRAP    5
#define SIGABRT    6
#define SIGIOT     6
#define SIGBUS     7
#define SIGFPE     8
#define SIGKILL    9
#define SIGUSR1   10
#define SIGSEGV   11
#define SIGUSR2   12
#define SIGPIPE   13
#define SIGALRM   14
#define SIGTERM   15
#define SIGSTKFLT 16
#define SIGCHLD   17
#define SIGCONT   18
#define SIGSTOP   19
#define SIGTSTP   20
#define SIGTTIN   21
#define SIGTTOU   22
#define SIGURG    23
/* SIGXCPU..SIGPROF: mono-threads-posix-signals.c 从这一段里挑三个
 * 实时性不敏感的信号当 suspend/resume/abort 的载体
 * （mono_threads_suspend_get_suspend_signal 等）。少一个就编不过。 */
#define SIGXCPU   24
#define SIGXFSZ   25
#define SIGVTALRM 26
#define SIGPROF   27
#define SIGWINCH  28
#define SIGIO     29
#define SIGPOLL   SIGIO
#define SIGPWR    30
#define SIGSYS    31
#define SIGUNUSED 31

/* 实时信号区间。Linux i386 上 SIGRTMIN 实际是 34（32/33 被 glibc 的
 * NPTL 私用），Mono 的 mono_threads_suspend_search_alternative_signal()
 * 会在这一段里找空位。 */
#define SIGRTMIN  34
#define SIGRTMAX  64

#define NSIG      65

/* ---- dispositions ---- */
typedef void (*__sighandler_t) (int);
#define SIG_DFL ((__sighandler_t) 0)
#define SIG_IGN ((__sighandler_t) 1)
#define SIG_ERR ((__sighandler_t)-1)

/* ---- sigprocmask() how ---- */
#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

/* ---- sigaction flags ---- */
#define SA_NOCLDSTOP 0x00000001
#define SA_NOCLDWAIT 0x00000002
#define SA_SIGINFO   0x00000004
#define SA_ONSTACK   0x08000000
#define SA_RESTART   0x10000000
#define SA_NODEFER   0x40000000
#define SA_RESETHAND 0x80000000

/* ---- sigaltstack ---- */
#define SS_ONSTACK   1
#define SS_DISABLE   2
#define MINSIGSTKSZ  2048
#define SIGSTKSZ     8192

typedef struct { unsigned long __bits[2]; } sigset_t;

typedef union sigval {
    int   sival_int;
    void *sival_ptr;
} sigval_t;

typedef struct siginfo {
    int       si_signo;
    int       si_errno;
    int       si_code;
    pid_t     si_pid;
    uid_t     si_uid;
    void     *si_addr;
    int       si_status;
    sigval_t  si_value;
} siginfo_t;

struct sigaction {
    union {
        __sighandler_t sa_handler;
        void (*sa_sigaction) (int, siginfo_t *, void *);
    } __sa_u;
    sigset_t sa_mask;
    int      sa_flags;
    void   (*sa_restorer) (void);
};
#define sa_handler   __sa_u.sa_handler
#define sa_sigaction __sa_u.sa_sigaction

typedef struct sigaltstack {
    void  *ss_sp;
    int    ss_flags;
    size_t ss_size;
} stack_t;

/* ---- machine context (Linux/i386 shaped) ---- */
typedef int greg_t;
#define NGREG 19
typedef greg_t gregset_t[NGREG];

enum {
    REG_GS = 0, REG_FS, REG_ES, REG_DS,
    REG_EDI,    REG_ESI, REG_EBP, REG_ESP,
    REG_EBX,    REG_EDX, REG_ECX, REG_EAX,
    REG_TRAPNO, REG_ERR, REG_EIP, REG_CS,
    REG_EFL,    REG_UESP, REG_SS
};

typedef struct {
    gregset_t     gregs;
    void         *fpregs;
    unsigned long oldmask;
    unsigned long cr2;
} mcontext_t;

/* Linux/i386 struct sigcontext.  mono-context.c reads it through the SC_*
 * macros in mono-context.h, whose default branch expects exactly these
 * lowercase field names (eax/ebx/.../eip) -- keep them verbatim. */
struct sigcontext {
    unsigned short gs, __gsh;
    unsigned short fs, __fsh;
    unsigned short es, __esh;
    unsigned short ds, __dsh;
    unsigned long  edi;
    unsigned long  esi;
    unsigned long  ebp;
    unsigned long  esp;
    unsigned long  ebx;
    unsigned long  edx;
    unsigned long  ecx;
    unsigned long  eax;
    unsigned long  trapno;
    unsigned long  err;
    unsigned long  eip;
    unsigned short cs, __csh;
    unsigned long  eflags;
    unsigned long  esp_at_signal;
    unsigned short ss, __ssh;
    void          *fpstate;
    unsigned long  oldmask;
    unsigned long  cr2;
};

typedef struct ucontext {
    unsigned long    uc_flags;
    struct ucontext *uc_link;
    stack_t          uc_stack;
    mcontext_t       uc_mcontext;
    sigset_t         uc_sigmask;
} ucontext_t;

/* ---- entry points (all stubs in Phase 0) ---- */
int  sigemptyset  (sigset_t *set);
int  sigfillset   (sigset_t *set);
int  sigaddset    (sigset_t *set, int signo);
int  sigdelset    (sigset_t *set, int signo);
int  sigismember  (const sigset_t *set, int signo);
int  sigprocmask  (int how, const sigset_t *set, sigset_t *oldset);
int  sigsuspend   (const sigset_t *mask);
int  sigaction    (int signo, const struct sigaction *act, struct sigaction *oact);
int  sigaltstack  (const stack_t *ss, stack_t *oss);
int  raise        (int signo);
int  kill         (pid_t pid, int signo);
__sighandler_t signal (int signo, __sighandler_t handler);

#endif /* PAL_SIGNAL_H */
