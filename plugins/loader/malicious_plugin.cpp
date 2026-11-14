/* =====================================================================
 *  malicious_plugin.cpp  -  「内鬼」插件的真实实现（阶段三/四）
 * ---------------------------------------------------------------------
 *  这不是伪代码：它被真的编译成一个独立目标文件，`nm -u` 里没有
 *  fopen / malloc / printf —— 因为它**只能**通过 EvilAbi 与宿主对话。
 *
 *  编译约束与内核一致：-ffreestanding -fno-exceptions -fno-rtti -nostdlib
 *
 *  它做四件坏事：
 *    1. 伪造野指针 0xDEADBEEF 并尝试写；
 *    2. 申请读 /etc/passwd（越权绝对路径）；
 *    3. 申请读 /sandbox/evil/../../etc/shadow（路径穿越）；
 *    4. 控制组：申请读自己沙盒里的 notes.txt（必须成功，证明不是“全拒”）。
 *
 *  并且在 1 之后，如果内核居然放行（rc == 1），代码会真的解引用
 *  ——那是“防线被穿透”的证据，宿主用 SIGSEGV/IST 兜底并计入
 *  raw_deref_reached。
 * ===================================================================== */
#include "malicious_plugin.h"

#ifndef NULL
#  define NULL 0
#endif

static EvilAbi* s_abi = NULL;

EvilAbi* evil_abi(void) { return s_abi; }

/* ------------------------------------------------------------ 小工具 */

/* 拼 sandbox_root + "/notes.txt"（不信任长度，显式截断）。 */
static void ev_join(char* dst, uint32_t cap, const char* root, const char* leaf)
{
    uint32_t o = 0;
    uint32_t i = 0;
    if (!dst || cap == 0) return;
    if (root) {
        while (root[i] && o + 1u < cap) dst[o++] = root[i++];
    }
    if (o + 1u < cap) dst[o++] = '/';
    i = 0;
    if (leaf) {
        while (leaf[i] && o + 1u < cap) dst[o++] = leaf[i++];
    }
    dst[o] = 0;
}

/* 把路径塞进 {"path":"..."} —— 只用于自测，不做通用转义。 */
static void ev_json_path(char* dst, uint32_t cap, const char* path)
{
    uint32_t o = 0;
    const char* pre = "{\"path\":\"";
    uint32_t i = 0;
    if (!dst || cap == 0) return;
    while (pre[i] && o + 1u < cap) dst[o++] = pre[i++];
    i = 0;
    while (path && path[i] && o + 1u < cap) dst[o++] = path[i++];
    if (o + 2u < cap) { dst[o++] = '"'; dst[o++] = '}'; }
    dst[o < cap ? o : cap - 1u] = 0;
}

/* =====================================================================
 *  加载期入口点：插件一被 load_plugin() 接受就立刻开始作恶
 * ===================================================================== */
int evil_entry(EvilAbi* abi, EvilReport* rep, const char* sandbox_root)
{
    s_abi = abi;
    if (rep) {
        rep->ptr_check_rc = 0;
        rep->ptr_check_errno = 0;
        rep->ptr_check_inner_rc = 0;
        rep->shadow_open_rc = 0;
        rep->shadow_open_errno = 0;
        rep->traversal_open_rc = 0;
        rep->legit_open_rc = 0;
        rep->raw_deref_reached = 0;
    }
    if (!abi) return -1;

    char out[256];
    char path[192];
    char args[224];

    /* --- 1. 伪造野指针：先过内核的指针闸门 ------------------------- */
    int rc = abi->check_ptr(abi->ctx, (const void*)(uintptr_t)0xDEADBEEFu,
                            8, EV_PROT_WRITE);
    if (rep) {
        rep->ptr_check_rc = rc;
        /* 立即采样：后面的 mcp_call 会把错误通道改写。 */
        rep->ptr_check_errno = abi->last_errno ? abi->last_errno(abi->ctx) : 0;
    }

    if (rc == 1) {
        /* 防线被穿透：真的写一下。宿主/内核的故障陷阱必须兜住。 */
        if (rep) rep->raw_deref_reached = 1;
        evil_raw_deref(0xDEADBEEFu);
    }

    /* --- 对照：自己沙盒里的指针必须放行 ---------------------------- */
    if (abi->sandbox_mem && abi->sandbox_len > 128u) {
        int ok = abi->check_ptr(abi->ctx,
                                (const char*)abi->sandbox_mem + 64,
                                16, EV_PROT_READ);
        if (rep) rep->ptr_check_inner_rc = ok;
    }

    /* --- 2. 越权绝对路径 ------------------------------------------ */
    out[0] = 0;
    rc = abi->mcp_call(abi->ctx, "fs.read", "{\"path\":\"/etc/passwd\"}",
                       out, (uint32_t)sizeof(out));
    if (rep) {
        rep->shadow_open_rc = rc;
        rep->shadow_open_errno = abi->last_errno ? abi->last_errno(abi->ctx) : 0;
    }

    /* --- 2b. 换一个更狠的目标：/etc/shadow -------------------------- */
    out[0] = 0;
    abi->mcp_call(abi->ctx, "fs.read", "{\"path\":\"/etc/shadow\"}",
                  out, (uint32_t)sizeof(out));

    /* --- 3. 路径穿越 ---------------------------------------------- */
    out[0] = 0;
    rc = abi->mcp_call(abi->ctx, "fs.read",
                       "{\"path\":\"/sandbox/evil/../../etc/shadow\"}",
                       out, (uint32_t)sizeof(out));
    if (rep) rep->traversal_open_rc = rc;

    /* --- 4. 对照组：自己沙盒里的文件必须能读 ---------------------- */
    ev_join(path, sizeof(path),
            (sandbox_root && sandbox_root[0]) ? sandbox_root : "/sandbox/evil",
            "notes.txt");
    ev_json_path(args, sizeof(args), path);
    out[0] = 0;
    rc = abi->mcp_call(abi->ctx, "fs.read", args, out, (uint32_t)sizeof(out));
    if (rep) rep->legit_open_rc = rc;

    return 0;
}

/* =====================================================================
 *  工具实现
 * ===================================================================== */
int evil_echo_tool(uint32_t h, const char* args, char* out, uint32_t outcap)
{
    (void)h; (void)args;
    if (!out || outcap < 12u) return EV_ERR_DENIED;
    const char* s = "{\"ok\":true}";
    uint32_t i = 0;
    while (s[i] && i + 1u < outcap) { out[i] = s[i]; i++; }
    out[i] = 0;
    return EV_OK;
}

int evil_block_tool(uint32_t h, const char* args, char* out, uint32_t outcap)
{
    (void)h; (void)args;
    EvilAbi* abi = s_abi;
    if (abi && abi->gate) {
        /* 原地等待宿主开门。带硬上限，绝不让内核永远卡在这里。 */
        uint64_t spins = 0;
        while (*abi->gate && spins < 2000000000ULL) {
            spins++;
#if defined(__GNUC__) || defined(__clang__)
            __atomic_signal_fence(__ATOMIC_SEQ_CST);
#endif
        }
    }
    return evil_echo_tool(h, args, out, outcap);
}

/* =====================================================================
 *  裸野指针 —— 唯一一条不经过任何闸门的路径
 * ---------------------------------------------------------------------
 *  写 0xDEADBEEF 在任何现代 OS 上都是 #PF。NexOS 的处理链路是
 *  IDT -> fault_common -> compat_on_fault -> longjmp（IST 独立故障栈），
 *  宿主测试里则是 SIGSEGV -> siglongjmp。两者语义完全一致：
 *  **故障被限制在插件上下文内，内核继续运行。**
 * ===================================================================== */
void evil_raw_deref(uint64_t fake_ptr)
{
    volatile unsigned char* p = (volatile unsigned char*)(uintptr_t)fake_ptr;
    *p = 0x41;
}
