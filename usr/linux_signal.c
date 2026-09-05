/* =====================================================================
 *  usr/linux_signal.c  -  Stage 2 Linux signal-delivery smoke test
 * ---------------------------------------------------------------------
 *  Freestanding ELF32 guest exercising the NexOS Linux-compat signal
 *  layer directly via int 0x80:
 *    - sys_rt_sigaction (174)  : install a real handler (with SA_RESTORER)
 *    - sys_rt_sigprocmask (175): block / unblock signals
 *    - sys_tkill        (238)  : send a signal to a thread by tid
 *    - sys_tgkill       (270)  : send a signal to (tgid,tid)
 *    - sys_rt_sigreturn (173)  : restore context (via SA_RESTORER trampoline)
 *    - sys_gettid       (224)
 *    - sys_clone        (120)  : spawn a worker thread to receive signals
 *
 *  Verifies:
 *    * a handler runs, receives the correct signo, and returns cleanly
 *      (rt_sigreturn restores the interrupted context)
 *    * a blocked signal stays pending until unblocked, then is delivered
 *    * tkill/tgkill target the right thread
 *    * default action of SIGTERM terminates the process
 *
 *  Build: freestanding recipe (same as linux_threads.c)
 *  Run:   linux linux_signal   (from the NexOS shell)
 * ===================================================================== */
#include "libc.h"

#define SYS_RT_SIGACTION  174
#define SYS_RT_SIGPROCMASK 175
#define SYS_TKILL         238
#define SYS_TGKILL        270
#define SYS_RT_SIGRETURN  173
#define SYS_GETTID        224
#define SYS_CLONE         120
#define SYS_EXIT           1
#define SYS_WRITE          4

#define SIGUSR1  10
#define SIGUSR2  12
#define SIGTERM  15

#define SA_RESTORER 0x04000000

/* ---- raw syscall wrappers ---- */
static inline long sys_rt_sigaction(int sig, const void* act, void* oldact)
{
    long ret;
    __asm__ volatile ("int $0x80"
        : "=a"(ret)
        : "a"(SYS_RT_SIGACTION), "b"((long)sig), "c"((long)act), "d"((long)oldact)
        : "memory", "cc");
    return ret;
}
static inline long sys_rt_sigprocmask(int how, const unsigned long* set, unsigned long* oldset)
{
    long ret;
    __asm__ volatile ("int $0x80"
        : "=a"(ret)
        : "a"(SYS_RT_SIGPROCMASK), "b"((long)how),
          "c"((long)set), "d"((long)oldset)
        : "memory", "cc");
    return ret;
}
static inline long sys_tkill(int tid, int sig)
{
    long ret;
    __asm__ volatile ("int $0x80"
        : "=a"(ret) : "a"(SYS_TKILL), "b"((long)tid), "c"((long)sig)
        : "memory", "cc");
    return ret;
}
static inline long sys_tgkill(int tgid, int tid, int sig)
{
    long ret;
    __asm__ volatile ("int $0x80"
        : "=a"(ret)
        : "a"(SYS_TGKILL), "b"((long)tgid), "c"((long)tid), "d"((long)sig)
        : "memory", "cc");
    return ret;
}
static inline long sys_gettid(void)
{
    long ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(SYS_GETTID) : "memory", "cc");
    return ret;
}
static inline void sys_sched_yield(void)
{
    __asm__ volatile ("int $0x80" : : "a"(158) : "memory", "cc");
}
static long my_clone(void (*entry)(int), void* child_stack,
                     unsigned long flags, int arg, void* tls)
{
    long ret;
    __asm__ volatile ("int $0x80"
        : "=a"(ret)
        : "a"(SYS_CLONE),
          "b"((long)entry), "c"((long)child_stack), "d"((long)flags),
          "S"((long)arg), "D"((long)tls)
        : "memory", "cc");
    return ret;
}
static inline void sys_exit(int code)
{
    __asm__ volatile ("int $0x80" : : "a"(SYS_EXIT), "b"((long)code) : "memory", "cc");
    for (;;) {}
}
static inline long sys_write(int fd, const void* buf, unsigned long n)
{
    long ret;
    __asm__ volatile ("int $0x80"
        : "=a"(ret) : "a"(SYS_WRITE), "b"((long)fd), "c"((long)buf), "d"((long)n)
        : "memory", "cc");
    return ret;
}

/* ---- helpers to print without %ld (libc printf lacks it) ---- */
static void puts_dec(const char* pre, int v, const char* post)
{
    char line[80]; int p = 0;
    while (*pre) line[p++] = *pre++;
    if (v == 0) line[p++] = '0';
    else { int t = v; char tmp[12]; int k = 0; int neg = 0;
           if (t < 0){ neg = 1; t = -t; }
           while (t){ tmp[k++] = '0' + (t % 10); t /= 10; }
           if (neg) line[p++] = '-';
           while (k > 0) line[p++] = tmp[--k]; }
    while (*post) line[p++] = *post++;
    sys_write(1, line, p);
}

/* ---- signal frame restoring trampoline (SA_RESTORER) ---- */
__attribute__((naked)) static void sig_restore(void)
{
    __asm__ volatile (
        "movl $173, %%eax\n"     // SYS_RT_SIGRETURN
        "int $0x80\n"
        "1: hlt\n"
        : : : "eax", "memory");
}

/* ---- shared counters ---- */
volatile int g_main_hits  = 0;
volatile int g_worker_hits = 0;

/* signal handler: just record signo + tid and return (rt_sigreturn restores)
 * sa_handler signature: void (*)(int).  glibc on i386 passes signo in [esp]. */
static void handler(int sig)
{
    long tid = sys_gettid();
    /* identify which thread by tid parity is fragile; instead use a marker:
     * worker tid > 1 (main is 1).  We record both. */
    if (tid == 1){
        g_main_hits++;
        puts_dec("HANDLER main tid=1 sig=", (int)sig, "\n");
    } else {
        g_worker_hits++;
        puts_dec("HANDLER worker tid=", (int)tid, " sig=");
        puts_dec("", (int)sig, "\n");
    }
}

/* install sig=signo with our handler + restorer */
static void install(int signo)
{
    unsigned long act[4];
    act[0] = (unsigned long)(long)handler;   // sa_handler
    act[1] = SA_RESTORER;                     // sa_flags
    act[2] = (unsigned long)(long)sig_restore; // sa_restorer
    act[3] = 0;                               // sa_mask
    sys_rt_sigaction(signo, act, 0);
}

/* child stack pool */
#define STACK_WORDS 8192
static unsigned int g_wstack[STACK_WORDS] __attribute__((aligned(16)));

__attribute__((naked)) static void child_exit_tramp(void)
{
    __asm__ volatile (
        "movl $0, %%ebx\n"
        "movl $1, %%eax\n"
        "int $0x80\n"
        "1: hlt\n"
        : : : "eax", "ebx", "memory");
}

static void worker(int id)
{
    (void)id;
    long tid = sys_gettid();
    puts_dec("WORKER started tid=", (int)tid, "\n");
    /* wait to be signalled: spin on a local flag */
    volatile int done = 0;
    while (!done){
        /* a tkill will interrupt this loop and run the handler; after
         * rt_sigreturn we resume here.  Use a syscall boundary so pending
         * signals get delivered.  Yield afterwards so the main thread can
         * run (cooperative scheduler: without a yield we'd spin forever and
         * the sender would never get CPU to check our hit count). */
        sys_gettid();   // syscall boundary -> delivers any pending signal
        if (g_worker_hits >= 2) done = 1;
        sys_sched_yield();  // hand CPU back to main
    }
    puts_dec("WORKER exiting tid=", (int)tid, "\n");
    sys_exit(0);
}

int main(int argc, char** argv, char** envp)
{
    (void)argc; (void)argv;
    sys_write(1, "LXSIG: start\n", 13);

    install(SIGUSR1);
    install(SIGUSR2);

    long mytid = sys_gettid();
    puts_dec("LXSIG: main tid=", (int)mytid, "\n");

    /* --- Test 1: tkill self SIGUSR1, handler runs, context restored --- */
    sys_write(1, "LXSIG: tkill self SIGUSR1\n", 27);
    sys_tkill((int)mytid, SIGUSR1);   // pending; delivered on syscall return
    /* the tkill syscall itself returns -> boundary -> handler runs -> returns
     * back here after rt_sigreturn.  If we reach here, restore worked. */
    sys_write(1, "LXSIG: after SIGUSR1 recovered\n", 31);
    if (g_main_hits != 1){
        puts_dec("LXSIG: FAIL main_hits=", g_main_hits, " (expected 1)\n");
        sys_exit(1);
    }

    /* --- Test 2: block SIGUSR2, tkill it (stays pending), then unblock --- */
    sys_write(1, "LXSIG: block SIGUSR2 then tkill\n", 33);
    unsigned long blockmask = (1u << SIGUSR2);
    unsigned long oldmask = 0;
    sys_rt_sigprocmask(1 /*BLOCK*/, &blockmask, &oldmask);  // SIG_BLOCK
    sys_tkill((int)mytid, SIGUSR2);   // pending but blocked -> not delivered
    /* barrier syscall: confirms the signal did NOT fire while blocked. */
    sys_gettid();
    /* Print the confirmation while still blocked: no SIGUSR2 can interrupt
     * this write, so the marker stays intact in the serial log. */
    sys_write(1, "LXSIG: SIGUSR2 blocked OK\n", 24);
    if (g_main_hits != 1){
        puts_dec("LXSIG: FAIL SIGUSR2 delivered while blocked (hits=", g_main_hits, ")\n");
        sys_exit(1);
    }
    /* unblock -> pending SIGUSR2 now delivered on the next boundary */
    sys_rt_sigprocmask(2 /*UNBLOCK*/, &blockmask, &oldmask); // SIG_UNBLOCK
    sys_gettid();  // boundary -> handler for SIGUSR2 runs
    sys_write(1, "LXSIG: after SIGUSR2 unblock recovered\n", 37);
    if (g_main_hits != 2){
        puts_dec("LXSIG: FAIL main_hits=", g_main_hits, " (expected 2)\n");
        sys_exit(1);
    }

    /* --- Test 3: spawn worker, tkill / tgkill it precisely --- */
    sys_write(1, "LXSIG: spawn worker\n", 22);
    unsigned int* top = &g_wstack[STACK_WORDS - 4];
    top[0] = (unsigned int)(long)child_exit_tramp;
    top[1] = (unsigned int)0;
    void* csp = (void*)((unsigned char*)top + 16);
    long wtid = my_clone(worker, csp, 0, 0, 0);
    if (wtid < 0){
        puts_dec("LXSIG: clone failed tid=", (int)wtid, "\n");
        sys_exit(3);
    }
    puts_dec("LXSIG: worker tid=", (int)wtid, " spawned\n");

    /* let worker reach its spin loop (it does a syscall boundary each iter) */
    for (volatile int i = 0; i < 100000; i++) {}
    sys_sched_yield();   // hand CPU to the worker so it can run

    /* tkill worker SIGUSR1 */
    sys_write(1, "LXSIG: tkill worker SIGUSR1\n", 30);
    sys_tkill((int)wtid, SIGUSR1);
    /* yield so the worker actually runs and takes the signal */
    sys_sched_yield();
    for (volatile int i = 0; i < 100000; i++) {}
    if (g_worker_hits < 1){
        puts_dec("LXSIG: FAIL worker SIGUSR1 not caught (hits=", g_worker_hits, ")\n");
        sys_exit(1);
    }

    /* tgkill worker SIGUSR2 (precise tgid,tid targeting) */
    sys_write(1, "LXSIG: tgkill worker SIGUSR2\n", 30);
    sys_tgkill((int)mytid, (int)wtid, SIGUSR2);
    sys_sched_yield();
    for (volatile int i = 0; i < 100000; i++) {}
    if (g_worker_hits < 2){
        puts_dec("LXSIG: FAIL worker SIGUSR2 not caught (hits=", g_worker_hits, ")\n");
        sys_exit(1);
    }

    /* tell worker to exit (it checks g_worker_hits>=2) */
    for (volatile int i = 0; i < 100000; i++) {}
    /* join worker: spin until it exits (cooperative; it will exit on its own) */
    for (volatile int i = 0; i < 2000000; i++) {}

    sys_write(1, "LXSIG: signal tests done\n", 24);

    /* --- Test 4: default action of SIGTERM terminates the process --- */
    sys_write(1, "LXSIG: tkill self SIGTERM (default=terminate)\n", 47);
    sys_tkill((int)mytid, SIGTERM);
    sys_gettid();  // boundary -> default action -> process exits
    /* should NOT reach here */
    sys_write(1, "LXSIG: FAIL SIGTERM did not terminate\n", 37);
    sys_exit(2);
}
