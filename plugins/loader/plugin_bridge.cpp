/* =====================================================================
 *  plugin_bridge.cpp  -  内核接线层实现
 * ---------------------------------------------------------------------
 *  依赖：
 *    plugin_core.{h,cpp}   —— 机制层（锁/影子页表/路由/生命周期）
 *    malicious_plugin.cpp  —— 与宿主破坏性测试**同一个**恶意插件 TU
 *    osapi.h               —— 错误通道契约
 *    clr.cpp               —— clr_str / clr_new_str（托管字符串）
 *
 *  本文件不含任何标准库头；字符串/数字输出全部手写。
 * ===================================================================== */
#include "plugin_bridge.h"
#include "plugin_core.h"
#include "malicious_plugin.h"
#include "osapi.h"
/* clr.h 已把 clr_str / clr_new_str / clr_register_icall 放进 extern "C" 块，
 * 是这两个函数的唯一权威声明（链接名 clr_str / clr_new_str，不带 _Z 前缀）。
 * 这里直接包含而不是手写 extern "C" 原型：手写版本一旦和头文件漂移
 * （曾经就漂过），生成的符号名会和 clr.cpp 对不上，check-k64 立刻报未定义。 */
#include "clr.h"

#ifndef NULL
#  define NULL 0
#endif

/* kernel64.cpp 里的 C++ 链接 serial_puts（注意：不是 extern "C"）。 */
void serial_puts(const char* s);

/* 注意：clr_str / clr_new_str 是 C 链接（声明在 clr.h 的 extern "C" 块里）。
 * 若在这里按 C++ 链接再声明一次，会生成 _Z11clr_new_strPKc / _Z7clr_stri
 * 两个 64 位内核并不存在的符号；check-k64 会直接判失败。 */

/* =====================================================================
 *  串口输出
 * ===================================================================== */
static void br_emit(const char* s)
{
    if (s) serial_puts(s);
}

static void br_say(const char* s)   { br_emit(s); }

static void br_int(int64_t v)
{
    char t[24];
    int  k = 0;
    int  neg = (v < 0);
    uint64_t mag = neg ? ((uint64_t)(-(v + 1)) + 1ULL) : (uint64_t)v;
    char c[2];
    c[1] = 0;
    if (mag == 0) { c[0] = '0'; br_emit(c); return; }
    while (mag > 0 && k < (int)sizeof(t)) {
        t[k++] = (char)('0' + (uint32_t)(mag % 10ULL));
        mag /= 10ULL;
    }
    if (neg) br_emit("-");
    while (k > 0) { c[0] = t[--k]; br_emit(c); }
}

static void br_hex(uint64_t v)
{
    static const char* hd = "0123456789abcdef";
    char t[18];
    int  k = 0;
    char c[2];
    c[1] = 0;
    if (v == 0) { br_say("0x0"); return; }
    while (v && k < 16) { t[k++] = hd[(uint32_t)(v & 0xFULL)]; v >>= 4; }
    br_say("0x");
    while (k > 0) { c[0] = t[--k]; br_emit(c); }
}

static int g_fail;
static int g_pass;

static void br_check(int cond, const char* name)
{
    if (cond) { g_pass++; br_say("[PLUGIN-CORE] PASS  "); }
    else      { g_fail++; br_say("[PLUGIN-CORE] FAIL  "); }
    br_say(name);
    br_say("\n");
}

/* 红线日志：只在“攻击被拦下”时打印，方便抓串口时一眼定位。 */
static void br_redline(const char* what, const char* detail,
                       uint64_t addr, int rc)
{
    br_say("[REDLINE] ");
    br_say(what);
    if (detail) { br_say("  "); br_say(detail); }
    if (addr)   { br_say("  addr="); br_hex(addr); }
    br_say("  rc=");
    br_int(rc);
    br_say("  errno=");
    br_int(osapi_get_last_error());
    br_say("  -> blocked, kernel alive\n");
}

/* =====================================================================
 *  恶意插件的沙盒页（影子页表里唯一被授予 RW 的那一段）
 * ===================================================================== */
static uint8_t g_evil_page[PC_MIN_SANDBOX] __attribute__((aligned(4096)));

/* =====================================================================
 *  插件工具实现（内核侧）
 * ===================================================================== */
static int k_fs_read(uint32_t h, const char* args, char* out, uint32_t outcap)
{
    (void)h;
    char path[PC_MAX_PATH];
    if (json_get_string(args ? args : "{}", "path", path, sizeof(path)) != PC_OK) {
        if (out && outcap) safe_strcpy(out, outcap, "{\"ok\":false}");
        return PC_ERR_BADARG;
    }
    /* 走到这里说明沙盒校验已通过 —— 真正的读取动作由 VFS 完成。 */
    safe_strcpy(out, outcap, "{\"ok\":true,\"bytes\":17}");
    return PC_OK;
}

static EvilAbi g_abi;

static int k_entry_tool(uint32_t h, const char* args, char* out, uint32_t outcap)
{
    (void)h; (void)args;
    EvilReport rep;
    evil_entry(&g_abi, &rep, "/sandbox/evil.k64");
    return evil_echo_tool(h, args, out, outcap);
}

static int k_block_tool(uint32_t h, const char* args, char* out, uint32_t outcap)
{
    return evil_block_tool(h, args, out, outcap);
}

/* ---- ABI 后端：直接落到 plugin_core 的检查点 ------------------------ */
static int br_check_ptr(void* ctx, const void* p, uint32_t len, uint32_t prot)
{
    return is_valid_user_ptr_h((uint32_t)(uintptr_t)ctx, p, len, prot);
}

static int br_mcp_call(void* ctx, const char* tool, const char* args,
                       char* out, uint32_t cap)
{
    return dispatch_mcp((uint32_t)(uintptr_t)ctx, tool, args, out, cap);
}

static int br_last_errno(void* ctx)
{
    (void)ctx;
    return osapi_get_last_error();
}

/* =====================================================================
 *  icall 结果缓冲
 * ===================================================================== */
static char s_out[PC_MAX_OUT];
static uint32_t s_demo_handle;
static int s_booted;

const char* pc_bridge_result_buffer(void) { return s_out; }
int pc_bridge_last_failures(void) { return g_fail; }

/* =====================================================================
 *  开机自检：现场加载真实恶意插件，逐条撞闸门
 * ===================================================================== */
static void br_kernel_breach_selftest(void)
{
    br_say("\n===== plugin_core kernel breach selftest (unsigned, real TU) =====\n");

    /* 清空沙盒页，避免上一轮残留被误当成“合法数据”。 */
    safe_memset(g_evil_page, 0, (uint32_t)sizeof(g_evil_page));

    PcManifest m;
    safe_memset(&m, 0, (uint32_t)sizeof(m));
    m.id             = "evil.k64";
    m.sandbox_root   = "/etc";                    /* 恶意申请，必须被忽略 */
    m.sandbox_base   = (uint64_t)(uintptr_t)g_evil_page;
    m.sandbox_size   = (uint32_t)sizeof(g_evil_page);
    m.capabilities   = PC_CAP_FS_READ | PC_CAP_FS_WRITE | PC_CAP_SYSINFO;
    m.quota          = PC_HARD_INFLIGHT;          /* 允许内层 mcp_call 重入 */
    m.abi_version    = 1;
    m.extra_pages    = NULL;
    m.extra_page_count = 0;

    uint32_t h = 0;
    int rc = load_plugin(&m, 0 /* is_signed = 0 */, &h);
    br_check(rc == PC_OK && h != 0, "load_plugin(unsigned malicious plugin)");

    PcPlugin snap;
    safe_memset(&snap, 0, (uint32_t)sizeof(snap));
    if (plugin_snapshot(h, &snap) == PC_OK) {
        br_check((snap.caps & PC_CAP_FS_WRITE) == 0,
                 "CAP_FS_WRITE stripped at load time");
        br_check(safe_streq(snap.sandbox_root, "/sandbox/evil.k64"),
                 "sandbox_root forced to /sandbox/evil.k64 (manifest said /etc)");
        br_check(snap.page_count == 1 && snap.pages[0].prot == PC_PROT_RW,
                 "shadow page table: exactly one RW page, no EXEC");
    } else {
        br_check(0, "plugin_snapshot(handle)");
    }

    /* 工具注册：fs.read 需要 FS_READ 能力 + 路径沙盒；evil.entry 无额外能力 */
    br_check(register_tool(h, "fs.read", k_fs_read, PC_CAP_FS_READ,
                           PC_TOOL_PATH_SANDBOX) == PC_OK,
             "register fs.read (PATH_SANDBOX)");
    br_check(register_tool(h, "evil.entry", k_entry_tool, 0, 0) == PC_OK,
             "register evil.entry");
    br_check(register_tool(h, "evil.block", k_block_tool, 0, 0) == PC_OK,
             "register evil.block");

    /* ABI：插件只能看到这三个函数指针 + 自己的沙盒页 */
    g_abi.ctx         = (void*)(uintptr_t)h;
    g_abi.sandbox_mem = g_evil_page;
    g_abi.sandbox_len = (uint32_t)sizeof(g_evil_page);
    g_abi.check_ptr   = br_check_ptr;
    g_abi.mcp_call    = br_mcp_call;
    g_abi.gate        = NULL;
    g_abi.last_errno  = br_last_errno;

    /* ---- 让插件自己作恶（与宿主测试完全同一份 evil_entry） ---------- */
    EvilReport rep;
    evil_entry(&g_abi, &rep, "/sandbox/evil.k64");

    if (rep.ptr_check_rc != 1) {
        br_redline("forged wild pointer 0xdeadbeef", "is_valid_user_ptr",
                   0xDEADBEEFu, rep.ptr_check_rc);
    }
    br_check(rep.ptr_check_rc == PC_ERR_DENIED,
             "A1 is_valid_user_ptr(0xDEADBEEF) == PC_ERR_DENIED");
    br_check(rep.ptr_check_errno == ERR_PERMDENIED,
             "A1b errno at refusal == ERR_PERMDENIED");
    br_check(rep.raw_deref_reached == 0,
             "A2 raw dereference never reached (guard fired first)");
    br_check(rep.ptr_check_inner_rc == 1,
             "A3 legit sandbox pointer still accepted (not a blanket deny)");

    if (rep.shadow_open_rc != PC_OK) {
        br_redline("open /etc/shadow", "sandbox prefix check",
                   0, rep.shadow_open_rc);
    }
    br_check(rep.shadow_open_rc == PC_ERR_SANDBOX,
             "B1 /etc/shadow -> PC_ERR_SANDBOX (ERR_PERMDENIED)");
    br_check(rep.traversal_open_rc == PC_ERR_SANDBOX,
             "B2 ../../etc/shadow -> PC_ERR_SANDBOX");
    br_check(rep.legit_open_rc == PC_OK,
             "B3 /sandbox/evil.k64/notes.txt still readable");

    /* ---- 再走一次公开入口，确认闸门在 dispatch 里也生效 ------------- */
    s_out[0] = 0;
    rc = dispatch_mcp(h, "fs.read", "{\"path\":\"/etc/shadow\"}",
                      s_out, (uint32_t)sizeof(s_out));
    br_check(rc == PC_ERR_SANDBOX, "public dispatch /etc/shadow -> PC_ERR_SANDBOX");
    br_say("[REDLINE] dispatch_mcp json: ");
    br_say(s_out);
    br_say("\n");

    s_out[0] = 0;
    rc = dispatch_mcp(h, "evil.entry", "{}", s_out, (uint32_t)sizeof(s_out));
    br_check(rc == PC_OK, "dispatch evil.entry -> callback ran (kernel alive)");

    /* 伪造指针参数直接进 dispatch（MCP 层也要挡一次） */
    s_out[0] = 0;
    rc = dispatch_mcp(h, "fs.read",
                      "{\"ptr\":3735928559,\"len\":16}",
                      s_out, (uint32_t)sizeof(s_out));
    br_check(rc == PC_ERR_BADARG, "dispatch missing path -> PC_ERR_BADARG");

    /* ---- 场景 D：卸载后旧句柄立刻失效 ------------------------------- */
    rc = unload_plugin(h);
    br_check(rc == PC_OK, "unload_plugin(handle)");
    s_out[0] = 0;
    rc = dispatch_mcp(h, "fs.read", "{\"path\":\"/sandbox/evil.k64/a\"}",
                      s_out, (uint32_t)sizeof(s_out));
    br_check(rc == PC_ERR_STALE, "stale handle -> PC_ERR_STALE");
    br_check(plugin_lookup(h) == NULL, "plugin_lookup(stale) == NULL");
    br_check(plugin_count() == 0, "plugin_count back to 0");

    s_demo_handle = 0;

    br_say("===== plugin_core kernel selftest: ");
    br_int(g_pass);
    br_say(" passed, ");
    br_int(g_fail);
    br_say(" failed =====\n");
}

/* =====================================================================
 *  开机入口（幂等）
 * ===================================================================== */
void pc_bridge_boot(void)
{
    if (s_booted) return;
    s_booted = 1;

    pc_set_emit_hook(br_emit);
    pc_core_init();
    br_say("\n[PLUGIN-CORE] bridge boot: core init ok, router slots=");
    br_int(PC_ROUTER_SLOTS);
    br_say(", plugins max=");
    br_int(PC_MAX_PLUGINS);
    br_say("\n");

    br_kernel_breach_selftest();
}

/* =====================================================================
 *  托管系统调用：NexOS.Plugin::Dispatch(handle, tool, args) -> string
 * ---------------------------------------------------------------------
 *  a[0] = 插件句柄（int）
 *  a[1] = 工具名（托管字符串）
 *  a[2] = args JSON（托管字符串）
 *  ecx  = out_result 走返回值（托管字符串），另外同一份字节也留在
 *         pc_bridge_result_buffer() 里，供零拷贝路径使用。
 * ===================================================================== */
int32_t osapi_ic_plugin_dispatch(int32_t* a)
{
    if (!s_booted) { osapi_set_last_error(ERR_BROKEN); return 0; }
    if (!a)        { osapi_set_last_error(ERR_BADARG); return 0; }

    uint32_t h = (uint32_t)a[0];
    const char* tool = a[1] ? clr_str(a[1]) : "";
    const char* args = a[2] ? clr_str(a[2]) : "{}";
    if (!tool || !tool[0]) { osapi_set_last_error(ERR_BADARG); return 0; }
    if (!args || !args[0]) args = "{}";

    s_out[0] = 0;
    int rc = dispatch_mcp(h, tool, args, s_out, (uint32_t)sizeof(s_out));

    int saved = osapi_get_last_error();
    int32_t str = clr_new_str(s_out);
    if (!str) {
        osapi_set_last_error(rc == PC_OK ? ERR_NOMEM : saved);
        return 0;
    }
    /* dispatch_mcp 已经设好 errno：成功 = ERR_OK，失败 = 映射后的 ERR_*。 */
    osapi_set_last_error(rc == PC_OK ? ERR_OK : saved);
    return str;
}

int32_t osapi_ic_plugin_count(int32_t* a)
{
    (void)a;
    int n = plugin_count();
    osapi_set_last_error(ERR_OK);
    return (int32_t)n;
}

int32_t osapi_ic_plugin_alive(int32_t* a)
{
    if (!a) { osapi_set_last_error(ERR_BADARG); return 0; }
    int alive = (plugin_lookup((uint32_t)a[0]) != NULL) ? 1 : 0;
    osapi_set_last_error(alive ? ERR_OK : ERR_STALE);
    return (int32_t)alive;
}
