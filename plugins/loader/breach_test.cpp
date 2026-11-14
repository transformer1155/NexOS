/* =====================================================================
 *  breach_test.cpp  -  阶段三（真实恶意插件）+ 阶段四（并发/热拔插）
 * ---------------------------------------------------------------------
 *  宿主可执行文件（Linux/WSL/MSYS）。它做内核做不到的两件事：
 *
 *   1. 真的执行裸野指针写入 `*(char*)0xDEADBEEF = 1`，用 SIGSEGV 处理器
 *      + siglongjmp 把故障收回 —— 与内核里
 *      IDT -> fault_common -> compat_on_fault -> longjmp(IST 栈) 同构。
 *      内核镜像里绝不会跑这一行（会 triple fault），这是宿主测试的价值。
 *
 *   2. 用 pthread 制造内核 SMP 才能复现的真并发：16 线程 × 10000 次，
 *      中途热拔插。
 *
 *  被测代码与内核**完全相同**：plugin_core.cpp + malicious_plugin.cpp。
 *  退出码 = 失败用例数。
 * ===================================================================== */
#include "plugin_core.h"
#include "malicious_plugin.h"
#include "osapi.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/resource.h>

#ifndef NULL
#  define NULL 0
#endif

/* =====================================================================
 *  osapi 错误通道的宿主桩（内核里由 osapi.cpp 提供）
 * ===================================================================== */
static int g_last_err = ERR_OK;

int  osapi_set_last_error(int code) { g_last_err = code; return code; }
int  osapi_get_last_error(void)     { return g_last_err; }
void osapi_clear_last_error(void)   { g_last_err = ERR_OK; }

/* =====================================================================
 *  输出
 * ===================================================================== */
static int g_pass;
static int g_fail;

static void say(const char* s) { if (s) { fputs(s, stdout); fflush(stdout); } }

static void say_int(long long v)
{
    char t[24];
    int k = 0;
    int neg = (v < 0);
    unsigned long long m = neg ? (unsigned long long)(-(v + 1)) + 1ull
                               : (unsigned long long)v;
    char c[2]; c[1] = 0;
    if (m == 0) { c[0] = '0'; say(c); return; }
    while (m && k < (int)sizeof(t)) { t[k++] = (char)('0' + (int)(m % 10)); m /= 10; }
    if (neg) say("-");
    while (k > 0) { c[0] = t[--k]; say(c); }
}

static void say_hex(unsigned long long v)
{
    static const char* hd = "0123456789abcdef";
    char t[18];
    int k = 0;
    char c[2]; c[1] = 0;
    if (!v) { say("0x0"); return; }
    while (v && k < 16) { t[k++] = hd[(int)(v & 0xF)]; v >>= 4; }
    say("0x");
    while (k > 0) { c[0] = t[--k]; say(c); }
}

static void check(int cond, const char* name)
{
    if (cond) { g_pass++; say("[BREACH] PASS  "); }
    else      { g_fail++; say("[BREACH] FAIL  "); }
    say(name);
    say("\n");
}

static void redline(const char* what, const char* detail,
                    unsigned long long addr, int rc)
{
    say("[REDLINE] ");
    say(what);
    if (detail) { say("  "); say(detail); }
    if (addr)   { say("  addr="); say_hex(addr); }
    say("  rc=");   say_int(rc);
    say("  errno="); say_int(osapi_get_last_error());
    say("  -> blocked, process alive\n");
}

/* =====================================================================
 *  故障隔离实验台
 * ---------------------------------------------------------------------
 *  内核里裸写 0xDEADBEEF 会走 #PF -> fault_common -> compat_on_fault ->
 *  longjmp（IST 独立故障栈），故障被限制在插件上下文内。
 *
 *  宿主侧**不能**用 sigsetjmp 复现：仓库根的 setjmp.h 覆盖了 libc 的
 *  <setjmp.h>（-I . 在前），而且 glibc 的 jmp_buf 布局不是可移植契约。
 *  所以这里用更贴近微内核语义的等价物：**在独立进程里执行野指针**。
 *  子进程被 SIGSEGV 杀死，父进程（= 内核/宿主）完好无损 —— 这正是
 *  “插件越界不得带走系统”的可执行定义。
 *  回归检查：WIFSIGNALED && WTERMSIG == SIGSEGV。
 * ===================================================================== */
static int fault_isolate(void (*fn)(void))
{
    pid_t pid = fork();
    if (pid < 0) return -1000;
    if (pid == 0) {
        /* 子进程：允许核心转储会污染构建目录，直接关掉。 */
        fn();
        _exit(0);                    /* 没崩 = 野指针没被 MMU 抓住 */
    }
    int status = 0;
    if (waitpid(pid, &status, 0) != pid) return -1001;
    if (WIFSIGNALED(status)) return WTERMSIG(status) + 1000;
    if (WIFEXITED(status))   return WEXITSTATUS(status);   /* 0 = 未触发故障 */
    return -1002;
}

/* 让 fault_isolate 接受无参函数指针。 */
static void evil_raw_deref_thunk(void)
{
    evil_raw_deref(0xDEADBEEFu);
}

/* =====================================================================
 *  恶意插件的沙盒页
 * ===================================================================== */
static unsigned char g_page[PC_MIN_SANDBOX] __attribute__((aligned(4096)));

/* 内核侧等价实现：沙盒校验通过后，真正读文件的地方 */
static int host_fs_read(uint32_t h, const char* args, char* out, uint32_t outcap)
{
    (void)h;
    char path[PC_MAX_PATH];
    if (json_get_string(args ? args : "{}", "path", path, sizeof(path)) != PC_OK) {
        if (out && outcap) safe_strcpy(out, outcap, "{\"ok\":false}");
        return PC_ERR_BADARG;
    }
    safe_strcpy(out, outcap, "{\"ok\":true,\"bytes\":17}");
    return PC_OK;
}

static EvilAbi   g_abi;
static uint32_t  g_h;

static int host_check_ptr(void* ctx, const void* p, uint32_t len, uint32_t prot)
{
    return is_valid_user_ptr_h((uint32_t)(uintptr_t)ctx, p, len, prot);
}

static int host_mcp_call(void* ctx, const char* tool, const char* args,
                         char* out, uint32_t cap)
{
    return dispatch_mcp((uint32_t)(uintptr_t)ctx, tool, args, out, cap);
}

static int host_last_errno(void* ctx)
{
    (void)ctx;
    return osapi_get_last_error();
}

/* =====================================================================
 *  阶段四：并发状态
 * ===================================================================== */
#define STRESS_THREADS 16
#define STRESS_ITERS   10000
#define STRESS_TOTAL   (STRESS_THREADS * STRESS_ITERS)
/* 热拔插触发点：全局第 48000 次调用完成时（约 30%）。 */
#define STRESS_MID     (STRESS_TOTAL * 3 / 10)

static volatile long g_progress;      /* 已完成的 dispatch 次数 */
static volatile int  g_started;       /* 已启动线程数 */
static volatile int  g_gate;          /* evil.block 的闸门 */
/* 确定性热拔插握手：第 STRESS_MID 次调用完成者举手，主线程卸载完成后放行。 */
static volatile int  g_mid_reached;
static volatile int  g_unplug_done;

typedef struct {
    int   tid;
    long  ok, sandbox, stale, other;
    int   err_rc;                     /* 第一个非预期返回码 */
} TStat;

static uint64_t g_ptr_legal;          /* &g_page[64]（64 位，禁止截断） */
static char     g_args_legal_path[96];
static char     g_args_evil_path[64];
static char     g_args_traversal[96];
static char     g_args_ptr_bad[64];
static char     g_args_ptr_ok[64];

static void* stress_main(void* vp)
{
    TStat* st = (TStat*)vp;
    __atomic_fetch_add(&g_started, 1, __ATOMIC_SEQ_CST);

    for (int i = 0; i < STRESS_ITERS; ++i) {
        char out[256];
        int rc;
        int kind = (i + st->tid) & 3;

        if (kind == 0) {
            rc = dispatch_mcp(g_h, "fs.read", g_args_legal_path,
                              out, (uint32_t)sizeof(out));
        } else if (kind == 1) {
            rc = dispatch_mcp(g_h, "fs.read", g_args_evil_path,
                              out, (uint32_t)sizeof(out));
        } else if (kind == 2) {
            rc = dispatch_mcp(g_h, "fs.read", g_args_traversal,
                              out, (uint32_t)sizeof(out));
        } else {
            rc = dispatch_mcp(g_h, "evil.ptr", g_args_ptr_ok,
                              out, (uint32_t)sizeof(out));
        }

        /* 判定规则（顺序不能反）：
         *   STALE  —— 句柄已被热拔插，任何 kind 都可能看到它，属预期；
         *   OK     —— kind 0/3 的正常结果（kind 1/2 若能读到就是防穿透失败）；*/
        if (rc == PC_ERR_STALE) {
            st->stale++;
        } else if (rc == PC_OK) {
            if (kind == 1 || kind == 2) {
                st->other++;                 /* 沙盒被穿透！ */
                if (!st->err_rc) st->err_rc = PC_ERR_SANDBOX;
            } else {
                st->ok++;
            }
        } else if (rc == PC_ERR_SANDBOX) {
            if (kind == 1 || kind == 2) st->sandbox++;
            else { st->other++; if (!st->err_rc) st->err_rc = rc; }
        } else {
            st->other++;
            if (!st->err_rc) st->err_rc = rc;
        }

        /* 确定性握手：恰好第 STRESS_MID 次完成的线程举手并原地等待，
         * 主线程在这段时间里卸载插件，然后放行。这样“卸载发生在
         * 请求进行中”不依赖调度时序，也不依赖主线程的优先级。 */
        if (__atomic_add_fetch(&g_progress, 1, __ATOMIC_SEQ_CST) == STRESS_MID) {
            __atomic_store_n(&g_mid_reached, 1, __ATOMIC_SEQ_CST);
            while (!__atomic_load_n(&g_unplug_done, __ATOMIC_SEQ_CST)) {
#if defined(__GNUC__) || defined(__clang__)
                __atomic_signal_fence(__ATOMIC_SEQ_CST);
#endif
            }
        }
    }
    return NULL;
}

/* 阻塞工具线程：制造 in-flight 窗口 */
static volatile int g_block_rc = 12345;

static void* block_once(void* vp)
{
    (void)vp;
    char out[128];
    int rc = dispatch_mcp(g_h, "evil.block", "{}", out, (uint32_t)sizeof(out));
    g_block_rc = rc;
    return NULL;
}

/* =====================================================================
 *  main
 * ===================================================================== */
int main(void)
{
    /* 死锁看门狗：300 s 内没跑完就由 SIGALRM 结束进程（退出码 142），
     * 而不是让 CI 永远挂着。 */
    alarm(300);

    /* 子进程里会故意崩：关掉 core dump，避免构建目录里出现 core 文件。 */
    {
        struct rlimit rl;
        rl.rlim_cur = 0;
        rl.rlim_max = 0;
        setrlimit(RLIMIT_CORE, &rl);
    }

    /* 注意：指针必须按 64 位打印。宿主是 PIE，沙盒页常落在
     * 0x5555_xxxx_xxxx —— 用 %u 会截断成 32 位，闸门会正确拒绝
     * 那个“错误的地址”，测试就会误判成闸门失效。 */
    char num[24];
    g_ptr_legal = (uint64_t)(uintptr_t)&g_page[64];
    snprintf(num, sizeof(num), "%llu", (unsigned long long)g_ptr_legal);

    snprintf(g_args_legal_path, sizeof(g_args_legal_path),
             "{\"path\":\"/sandbox/evil.host/a.txt\"}");
    snprintf(g_args_evil_path, sizeof(g_args_evil_path),
             "{\"path\":\"/etc/passwd\"}");
    snprintf(g_args_traversal, sizeof(g_args_traversal),
             "{\"path\":\"/sandbox/evil.host/../../etc/shadow\"}");
    snprintf(g_args_ptr_bad, sizeof(g_args_ptr_bad),
             "{\"ptr\":3735928559,\"len\":16}");
    snprintf(g_args_ptr_ok, sizeof(g_args_ptr_ok),
             "{\"ptr\":%s,\"len\":16}", num);

    say("\n===== NexOS plugin_core breach test (host, real malicious TU) =====\n");

    pc_core_init();

    /* =================================================================
     *  阶段三：加载未签名恶意插件并触发它的逻辑
     * =============================================================== */
    say("\n--- 阶段三：恶意未签名插件 ---\n");

    PcManifest m;
    memset(&m, 0, sizeof(m));
    m.id               = "evil.host";
    m.sandbox_root     = "/etc";                    /* 恶意申请 */
    m.sandbox_base     = (uint64_t)(uintptr_t)g_page;
    m.sandbox_size     = (uint32_t)sizeof(g_page);
    m.capabilities     = PC_CAP_FS_READ | PC_CAP_FS_WRITE;
    m.quota            = PC_HARD_INFLIGHT;
    m.abi_version      = 1;

    int rc = load_plugin(&m, 0 /* unsigned */, &g_h);
    check(rc == PC_OK && g_h != 0, "load_plugin(malicious, is_signed=0)");

    PcPlugin snap;
    memset(&snap, 0, sizeof(snap));
    check(plugin_snapshot(g_h, &snap) == PC_OK, "plugin_snapshot(handle)");
    check((snap.caps & PC_CAP_FS_WRITE) == 0,
          "unsigned -> CAP_FS_WRITE stripped");
    check(safe_streq(snap.sandbox_root, "/sandbox/evil.host"),
          "manifest said /etc, kernel forced /sandbox/evil.host");
    check(snap.page_count == 1 && snap.pages[0].prot == PC_PROT_RW,
          "shadow page table: one RW page, no EXEC");
    check(snap.pages[0].base == (uint64_t)(uintptr_t)g_page,
          "shadow page base == host-provided sandbox");

    check(register_tool(g_h, "fs.read", host_fs_read, PC_CAP_FS_READ,
                        PC_TOOL_PATH_SANDBOX) == PC_OK,
          "register fs.read (PATH_SANDBOX)");
    check(register_tool(g_h, "evil.ptr", evil_echo_tool, 0,
                        PC_TOOL_USER_PTR) == PC_OK,
          "register evil.ptr (USER_PTR)");
    check(register_tool(g_h, "evil.block", evil_block_tool, 0, 0) == PC_OK,
          "register evil.block");

    g_abi.ctx         = (void*)(uintptr_t)g_h;
    g_abi.sandbox_mem = g_page;
    g_abi.sandbox_len = (uint32_t)sizeof(g_page);
    g_abi.check_ptr   = host_check_ptr;
    g_abi.mcp_call    = host_mcp_call;
    g_abi.gate        = &g_gate;
    g_abi.last_errno  = host_last_errno;

    /* ---- 插件的加载期入口点：它自己作恶 ---------------------------- */
    EvilReport rep;
    evil_entry(&g_abi, &rep, "/sandbox/evil.host");

    if (rep.ptr_check_rc != 1)
        redline("A: forged pointer 0xdeadbeef", "is_valid_user_ptr",
                0xDEADBEEFull, rep.ptr_check_rc);
    check(rep.ptr_check_rc == PC_ERR_DENIED,
          "A1 wild pointer -> PC_ERR_DENIED (kernel would survive)");
    check(rep.ptr_check_errno == ERR_PERMDENIED,
          "A2 errno at the moment of refusal == ERR_PERMDENIED");
    check(rep.raw_deref_reached == 0,
          "A3 raw deref never reached (guard fired first)");
    check(rep.ptr_check_inner_rc == 1,
          "A4 legit sandbox pointer still accepted");

    if (rep.shadow_open_rc != PC_OK)
        redline("B: open /etc/shadow", "sandbox prefix", 0, rep.shadow_open_rc);
    check(rep.shadow_open_rc == PC_ERR_SANDBOX,
          "B1 /etc/shadow -> PC_ERR_SANDBOX");
    check(rep.shadow_open_errno == ERR_PERMDENIED,
          "B1b errno at the moment of refusal == ERR_PERMDENIED");
    check(rep.traversal_open_rc == PC_ERR_SANDBOX,
          "B2 ../../etc/shadow -> PC_ERR_SANDBOX");
    check(rep.legit_open_rc == PC_OK,
          "B3 sandbox file still readable (not a blanket deny)");

    /* ---- 真的裸解引用：证明“没拦住”也不会带走内核 ------------------ */
    say("\n--- 阶段三补充：绕过闸门的裸野指针（故障隔离）---\n");
    {
        /* 子进程里真的写 0xDEADBEEF。内核侧的等价链路是
         * IDT -> fault_common -> compat_on_fault -> longjmp(IST)。 */
        int sig = fault_isolate(evil_raw_deref_thunk);
        if (sig == 1000 + SIGSEGV || sig == 1000 + SIGBUS) {
            check(1, "raw dereference killed the plugin context only (SIGSEGV)");
            say("[REDLINE] raw write to 0xdeadbeef faulted the plugin process, host alive\n");
        } else {
            check(0, "raw dereference did not fault (guard should not matter here)");
            say("[BREACH] fault_isolate returned ");
            say_int(sig);
            say("\n");
        }
    }

    /* 陷阱之后内核状态必须还能用 */
    char out[512];
    out[0] = 0;
    rc = dispatch_mcp(g_h, "fs.read", "{\"path\":\"/sandbox/evil.host/x\"}",
                      out, sizeof(out));
    check(rc == PC_OK, "core still functional after trapped fault");

    /* ---- 伪造指针走 public dispatch -------------------------------- */
    out[0] = 0;
    rc = dispatch_mcp(g_h, "evil.ptr", g_args_ptr_bad, out, sizeof(out));
    if (rc != PC_OK) redline("C: dispatch forged user ptr", "USER_PTR gate",
                             0xDEADBEEFull, rc);
    check(rc == PC_ERR_DENIED, "C1 dispatch forged ptr -> PC_ERR_DENIED");

    out[0] = 0;
    rc = dispatch_mcp(g_h, "evil.ptr", g_args_ptr_ok, out, sizeof(out));
    check(rc == PC_OK, "C2 dispatch legit sandbox ptr -> PC_OK");

    /* =================================================================
     *  阶段四：16 线程 × 10000 次 + 中途热拔插
     * =============================================================== */
    say("\n--- 阶段四：16 线程 x 10000 次并发 + 中途热拔插 ---\n");

    /* 4a. in-flight 时 unload 必须 BUSY（确定性用例先跑）
     * ---------------------------------------------------------------
     * evil.block 会在插件里自旋等待 g_gate 归零，于是 inflight 稳定为 1。
     * 等待窗口用墙钟（2 s），不用迭代次数 —— 否则一台慢/超卖的 VM 上
     * “线程还没被调度”会被误读成“锁没生效”。 */
    {
        pthread_t th;
        g_gate = 1;
        g_block_rc = 12345;
        pthread_create(&th, NULL, block_once, NULL);

        int saw_inflight = 0;
        struct timespec w0, w1;
        clock_gettime(CLOCK_MONOTONIC, &w0);
        for (;;) {
            PcPlugin s2;
            if (plugin_snapshot(g_h, &s2) == PC_OK && s2.inflight > 0) {
                saw_inflight = 1;
                break;
            }
            clock_gettime(CLOCK_MONOTONIC, &w1);
            if ((w1.tv_sec - w0.tv_sec) * 1000000000L +
                (w1.tv_nsec - w0.tv_nsec) > 2000000000L) break;
        }
        check(saw_inflight, "evil.block is in-flight (inflight > 0)");

        int busy = unload_plugin(g_h);
        if (busy != PC_ERR_BUSY) {
            say("[BREACH] unload while in-flight returned ");
            say_int(busy);
            say(" (expected ");
            say_int(PC_ERR_BUSY);
            say(")\n");
        }
        check(busy == PC_ERR_BUSY, "unload while in-flight -> PC_ERR_BUSY");

        g_gate = 0;
        pthread_join(th, NULL);
        if (g_block_rc != PC_OK) {
            say("[BREACH] evil.block dispatch rc=");
            say_int(g_block_rc);
            say("\n");
        }
        check(g_block_rc == PC_OK, "blocked tool completed after gate opened");
        check(unload_plugin(g_h) == PC_OK, "unload after inflight drains -> OK");

        /* 重新加载，继续压力测试 */
        rc = load_plugin(&m, 0, &g_h);
        check(rc == PC_OK && g_h != 0, "reload for stress phase");
        register_tool(g_h, "fs.read", host_fs_read, PC_CAP_FS_READ,
                      PC_TOOL_PATH_SANDBOX);
        register_tool(g_h, "evil.ptr", evil_echo_tool, 0, PC_TOOL_USER_PTR);
        register_tool(g_h, "evil.block", evil_block_tool, 0, 0);
        g_abi.ctx = (void*)(uintptr_t)g_h;
    }

    /* 4b. 16 线程 × 10000 次，跑到约 30% 时热拔插
     * ---------------------------------------------------------------
     * 线程**不提前退出**：每个线程都打满 STRESS_ITERS 次。这样
     * “拔插前 = OK/SANDBOX，拔插后 = STALE” 是确定性切分，
     * 总完成数必须精确等于 160000 —— 少一次就说明丢请求/死锁。
     * at_unplug 是拔插瞬间的进度快照，只用于报告，不作为断言依据
     * （多线程下读到的值天然有抖动）。 */
    g_progress     = 0;
    g_started      = 0;
    g_mid_reached  = 0;
    g_unplug_done  = 0;

    TStat stats[STRESS_THREADS];
    pthread_t tids[STRESS_THREADS];
    memset(stats, 0, sizeof(stats));
    for (int t = 0; t < STRESS_THREADS; ++t) {
        stats[t].tid = t;
        if (pthread_create(&tids[t], NULL, stress_main, &stats[t]) != 0) {
            check(0, "pthread_create");
            return g_fail;
        }
    }

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    /* 等第 48000 次调用完成（那一个线程会停住等我们）。30 s 上限只是
     * 为了不挂死：真要到上限，说明并发路径上有非常严重的问题。 */
    {
        struct timespec wt0, wt1;
        clock_gettime(CLOCK_MONOTONIC, &wt0);
        while (!__atomic_load_n(&g_mid_reached, __ATOMIC_SEQ_CST)) {
            clock_gettime(CLOCK_MONOTONIC, &wt1);
            if ((wt1.tv_sec - wt0.tv_sec) * 1000000000L +
                (wt1.tv_nsec - wt0.tv_nsec) > 30000000000L) break;
        }
    }

    long at_unplug = __atomic_load_n(&g_progress, __ATOMIC_SEQ_CST);
    say("[BREACH] hot-plug trigger at ");
    say_int(at_unplug);
    say(" / "); say_int(STRESS_TOTAL); say(" dispatches\n");

    /* ---- 真·热拔插：线程还在跑的时候卸载 ---------------------------
     * dispatch_mcp 与 unload_plugin 互斥：inflight > 0 时 unload 返回
     * PC_ERR_BUSY。所以这里是“重试直到拿到一个空窗”的循环 —— 真实
     * 热拔插的语义就是拔不掉就再拔，而不是先停机。若 5 s 内始终拿不到
     * PC_OK，说明自旋锁/引用计数泄漏，由下面的断言报错而不是挂死。 */
    int  unplug_rc  = PC_ERR_BUSY;
    long unplug_try = 0;
    {
        struct timespec d0, d1;
        clock_gettime(CLOCK_MONOTONIC, &d0);
        for (;;) {
            unplug_try++;
            unplug_rc = unload_plugin(g_h);
            if (unplug_rc == PC_OK)       break;
            if (unplug_rc != PC_ERR_BUSY) break;   /* 真错误，立即上报 */
            clock_gettime(CLOCK_MONOTONIC, &d1);
            if ((d1.tv_sec - d0.tv_sec) * 1000000000L +
                (d1.tv_nsec - d0.tv_nsec) > 5000000000L) break;
        }
    }
    check(unplug_rc == PC_OK,
          "unload_plugin succeeded while 16 threads still dispatching");

    /* 放行停在 48000 处的线程；之后它和其余线程的所有请求都必须
     * 返回 PC_ERR_STALE —— 这就是“拔插后旧请求自动失效”的确定性证据。 */
    __atomic_store_n(&g_unplug_done, 1, __ATOMIC_SEQ_CST);

    int joined = 0;
    for (int t = 0; t < STRESS_THREADS; ++t) {
        if (pthread_join(tids[t], NULL) == 0) joined++;
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);

    long ok = 0, sand = 0, other = 0, stale_total = 0;
    int  first_bad = 0;
    for (int t = 0; t < STRESS_THREADS; ++t) {
        ok    += stats[t].ok;
        sand  += stats[t].sandbox;
        stale_total += stats[t].stale;
        other += stats[t].other;
        if (!first_bad && stats[t].err_rc) first_bad = stats[t].err_rc;
    }
    long done = ok + sand + other + stale_total;

    long us = (t1.tv_sec - t0.tv_sec) * 1000000L +
              (t1.tv_nsec - t0.tv_nsec) / 1000L;

    check(joined == STRESS_THREADS, "no deadlock: 16/16 threads joined");
    check(other == 0, "no unexpected return code under contention");
    if (other) {
        say("[BREACH] first unexpected rc=");
        say_int(first_bad);
        say("\n");
    }
    /* 防穿透：并发路径上也不能出现 kind 1/2 返回 OK（那会被记进 other）。
     * 同时正向证据：合法请求和越权请求都真的发生过。 */
    check(ok > 0 && sand > 0, "both legal and malicious requests observed");
    check(stale_total > 0, "old handle dispatches observed PC_ERR_STALE after unplug");
    /* 精确不变量：16 x 10000 次全部有结果 —— 丢一次就是死锁/丢锁。 */
    check(done == STRESS_TOTAL, "exactly 160000 dispatches completed (none lost)");
    check(at_unplug > 0 && at_unplug < STRESS_TOTAL,
          "hot-plug happened while the load was still running");

    say("[BREACH] completed "); say_int(done);
    say(" dispatches in "); say_int(us / 1000);
    say(" ms  (ok="); say_int(ok);
    say(" sandbox="); say_int(sand);
    say(" stale="); say_int(stale_total);
    say(" other="); say_int(other);
    say(" unload_tries="); say_int(unplug_try); say(")\n");

    /* 4c. 卸载后旧句柄全部失效 */
    uint32_t oldh = g_h;
    /* 插件已在 4b 的热拔插里卸载完毕；重复卸载必须幂等地被拒（STALE），
     * 而不是二次释放槽位。 */
    rc = unload_plugin(oldh);
    check(rc == PC_ERR_STALE, "re-unload of unplugged handle -> PC_ERR_STALE");
    check(plugin_count() == 0, "plugin_count() == 0 after hot-unplug");

    int stale_seen = 0, stale_bad = 0;
    for (int i = 0; i < 256; ++i) {
        char o2[128];
        int r2 = dispatch_mcp(oldh, "fs.read", g_args_legal_path,
                              o2, sizeof(o2));
        if (r2 == PC_ERR_STALE) stale_seen++;
        else stale_bad++;
    }
    check(stale_seen == 256 && stale_bad == 0,
          "256/256 old-handle dispatches -> PC_ERR_STALE");
    check(plugin_lookup(oldh) == NULL, "plugin_lookup(old) == NULL");

    /* 槽位复用 + 代际递增 */
    rc = load_plugin(&m, 0, &g_h);
    check(rc == PC_OK && g_h != 0 && g_h != oldh,
          "slot reused with a new generation");
    register_tool(g_h, "fs.read", host_fs_read, PC_CAP_FS_READ,
                  PC_TOOL_PATH_SANDBOX);
    {
        char o3[128];
        rc = dispatch_mcp(oldh, "fs.read", g_args_legal_path, o3, sizeof(o3));
        check(rc == PC_ERR_STALE, "old handle still stale after slot reuse");
        rc = dispatch_mcp(g_h, "fs.read", g_args_legal_path, o3, sizeof(o3));
        check(rc == PC_OK, "new handle works");
    }
    unload_plugin(g_h);

    say("\n===== breach test: ");
    say_int(g_pass);
    say(" passed, ");
    say_int(g_fail);
    say(" failed =====\n");

    return g_fail;
}
