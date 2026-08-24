/* =====================================================================
 *  usr/linux_threads.c  -  Stage 1 Linux thread layer smoke test
 * ---------------------------------------------------------------------
 *  Freestanding ELF32 guest that exercises the NexOS Linux-compat
 *  threading syscalls directly via int 0x80:
 *    - sys_clone   (120)  : spawn N worker threads
 *    - sys_gettid  (224)  : report each thread's tid
 *    - sys_sched_yield (158): cooperative yield
 *    - sys_futex   (240)  : FUTEX_WAIT / FUTEX_WAKE (join barrier)
 *    - sys_exit    (1)    : per-thread exit
 *
 *  All threads share a single counter and a "done" counter.  Because the
 *  scheduler is COOPERATIVE (no preemption), the increment loop has no
 *  syscalls and is therefore atomic: every worker counts to N and the
 *  final counter must equal N_THREADS * PER_THREAD.  A futex-based join
 *  barrier proves FUTEX_WAIT/WAKE actually park and wake threads.
 *
 *  Build: same freestanding recipe as hello.nex / python.
 *  Run:   linux linux_threads   (from the NexOS shell)
 * ===================================================================== */
#include "libc.h"

#define SYS_CLONE        120
#define SYS_SCHED_YIELD  158
#define SYS_GETTID       224
#define SYS_FUTEX        240
#define SYS_EXIT           1
#define SYS_WRITE          4

#define CLONE_SETTLS  0x00020000

/* ---- raw syscall wrappers (preserve callee-saved regs per ABI) ---- */
static inline long sys_clone(unsigned long flags, void* child_stack,
                             void* arg, void* tls)
{
    long ret;
    __asm__ volatile ("int $0x80"
        : "=a"(ret)
        : "a"(SYS_CLONE), "b"((long)0 /*entry set by guest below*/),
          "c"((long)child_stack), "d"((long)flags), "S"((long)arg), "D"((long)tls)
        : "memory", "cc");
    return ret;
}

/* Our clone ABI (NexOS flavour) passes the child ENTRY in a separate
 * register than Linux; we wrap it so the guest controls child_entry. */
static long my_clone(void (*entry)(int), void* child_stack,
                     unsigned long flags, int arg, void* tls)
{
    long ret;
    __asm__ volatile ("int $0x80"
        : "=a"(ret)
        : "a"(SYS_CLONE),
          "b"((long)entry),        /* ebx = child_entry */
          "c"((long)child_stack),  /* ecx = child_stack (TOP) */
          "d"((long)flags),        /* edx = flags */
          "S"((long)arg),          /* esi = arg */
          "D"((long)tls)           /* ebp = tls_base (if CLONE_SETTLS) */
        : "memory", "cc");
    return ret;
}

static inline long sys_gettid(void)
{
    long ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(SYS_GETTID) : "memory", "cc");
    return ret;
}

static inline long sys_sched_yield(void)
{
    long ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(SYS_SCHED_YIELD) : "memory", "cc");
    return ret;
}

static inline long sys_futex(volatile int* uaddr, int op, int val, int n)
{
    long ret;
    __asm__ volatile ("int $0x80"
        : "=a"(ret)
        : "a"(SYS_FUTEX),
          "b"((long)uaddr), "c"((long)op), "d"((long)val), "S"((long)0), "D"((long)n)
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

/* ---- shared state (single address space, all threads see it) ---- */
#define N_THREADS   4
#define PER_THREAD  50000
volatile int   g_counter = 0;     /* every worker adds PER_THREAD */
volatile int   g_done    = 0;     /* workers increment on exit */
volatile int   g_barrier = 0;     /* futex join var */

/* child stack pool: each thread gets its own mmap'd stack.
 * We carve them out of a static array big enough for frame + stack.
 * child_stack TOP must leave >= 44 bytes below for the kernel's frame. */
#define STACK_WORDS  (8192)
static unsigned int g_stacks[N_THREADS][STACK_WORDS] __attribute__((aligned(16)));

/* trampoline the child returns into -> sys_exit(0) */
__attribute__((naked)) static void child_exit_tramp(void)
{
    __asm__ volatile (
        "movl $0, %%ebx\n"
        "movl $1, %%eax\n"
        "int $0x80\n"
        "1: hlt\n"
        : : : "eax", "ebx", "memory");
}

/* worker: count up, mark done, signal barrier, exit */
static void worker(int id)
{
    /* a tiny bit of per-thread work that does NOT call a syscall, so it
     * is atomic under cooperative scheduling */
    int i;
    for (i = 0; i < PER_THREAD; i++) g_counter++;

    /* print tid so the headless test can see interleaved scheduling */
    char line[64];
    int  p = 0;
    const char* pre = "THREAD tid=";
    while (*pre) line[p++] = *pre++;
    /* decimal tid */
    long tid = sys_gettid();
    if (tid == 0) { line[p++]='0'; } else {
        long t = tid; char tmp[12]; int k=0;
        if (t==0) tmp[k++]='0'; else while(t){ tmp[k++]='0'+(t%10); t/=10; }
        while(k>0) line[p++] = tmp[--k];
    }
    const char* mid = " done c=";
    while (*mid) line[p++] = *mid++;
    long c = g_counter;
    if (c==0){ line[p++]='0'; } else { long cc=c; char tmp[12]; int k=0;
        while(cc){ tmp[k++]='0'+(cc%10); cc/=10; } while(k>0) line[p++]=tmp[--k]; }
    line[p++]='\n';
    sys_write(1, line, p);

    /* mark done and wake the joiner */
    g_done++;
    __sync_fetch_and_add((int*)&g_barrier, 1);
    sys_futex((volatile int*)&g_barrier, 1 /*WAKE*/, 0, 1 /*wake 1*/);

    sys_exit(0);
}

int main(int argc, char** argv, char** envp)
{
    (void)argc; (void)argv;
    printf("LXTHREADS: start, spawning %d workers\n", N_THREADS);

    int i;
    for (i = 0; i < N_THREADS; i++) {
        /* set up child stack top; kernel frame lives at top-44, so keep
         * 16 bytes below for [exit_tramp][arg] */
        unsigned int* top = &g_stacks[i][STACK_WORDS - 4]; /* leave room */
        top[0] = (unsigned int)(long)child_exit_tramp;  /* return addr */
        top[1] = (unsigned int)i;                         /* arg */
        void* csp = (void*)((unsigned char*)top + 16);    /* top of stack */
        long tid = my_clone(worker, csp, 0, i, 0);
        if (tid < 0) {
            printf("LXTHREADS: clone failed tid=%d\n", (int)tid);
            return 3;
        }
        printf("LXTHREADS: spawned worker tid=%d\n", (int)tid);
    }

    /* join: spin-yield until all workers reported done, then wake any
     * parked joiner (ourselves) via the barrier futex */
    while (g_done < N_THREADS) {
        sys_sched_yield();
    }
    /* all done: sanity check the counter */
    printf("LXTHREADS: all %d workers exited, g_done=%d\n", N_THREADS, (int)g_done);

    long expect = (long)N_THREADS * (long)PER_THREAD;
    if (g_counter == expect) {
        printf("LXTHREADS: PASS counter=%d (expected %d)\n", (int)g_counter, (int)expect);
        sys_exit(0);
    } else {
        printf("LXTHREADS: FAIL counter=%d expected %d\n", (int)g_counter, (int)expect);
        sys_exit(1);
    }
}
