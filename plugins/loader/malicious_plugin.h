/* =====================================================================
 *  malicious_plugin.h  -  「内鬼」插件的 ABI 契约（阶段三/四 共用）
 * ---------------------------------------------------------------------
 *  这是恶意插件能看到的**全部**世界：一张函数指针表 + 一行 ctx。
 *  没有 libc（没有 malloc/fopen/printf），没有内核符号，没有地址空间特权。
 *
 *  为什么这很重要
 *  -------------------------------------------------------------------
 *  “插件调 fopen("/etc/shadow")” 在 NexOS 上之所以能被拦住，不是因为我们
 *  给 fopen 打了补丁，而是因为**插件里根本没有 fopen**：任何文件访问都必须
 *  走宿主提供的 mcp_call()/fs_open()，也就是内核的 dispatch_mcp()。
 *  控制点等于必经之路，这是微内核式能力模型的前提。
 *
 *  同一份实现同时编译进：
 *    - 宿主破坏性测试  plugins/loader/breach_test.cpp
 *    - 内核桥接模块    plugins/loader/plugin_bridge.cpp
 *  两边只替换 ABI 表的后端实现，插件代码一个字节都不改。
 * ===================================================================== */
#ifndef NEX_MALICIOUS_PLUGIN_H
#define NEX_MALICIOUS_PLUGIN_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------ 权限位 */
#define EV_PROT_READ   0x1u
#define EV_PROT_WRITE  0x2u
#define EV_PROT_EXEC   0x4u
#define EV_PROT_RW     (EV_PROT_READ | EV_PROT_WRITE)

/* 宿主返回码（与 plugin_core 的 PC_ERR_* 对齐） */
#define EV_OK           0
#define EV_ERR_SANDBOX (-8)
#define EV_ERR_DENIED  (-5)
#define EV_ERR_STALE   (-4)
#define EV_ERR_BUSY   (-10)

typedef struct EvilAbi {
    void* ctx;

    /* 宿主分配给本插件的沙盒页（影子页表里唯一被授予 RW 的那一段）。 */
    void*    sandbox_mem;
    uint32_t sandbox_len;

    /* is_valid_user_ptr()：1 = 合法，负值 = 拒绝。 */
    int (*check_ptr)(void* ctx, const void* p, uint32_t len, uint32_t prot);

    /* 内核 MCP 网关：等价于内核对 dispatch_mcp() 的封装。
     * 返回 plugin_core 的 PC_ERR_*；out 里是 JSON。 */
    int (*mcp_call)(void* ctx, const char* tool, const char* args,
                    char* out, uint32_t cap);

    /* 测试闸门：非 0 时工具函数原地等待（用于“热拔插时仍有 in-flight”）。 */
    volatile int* gate;

    /* 读取宿主/内核的错误通道（等价于 osapi_get_last_error）。 */
    int (*last_errno)(void* ctx);
} EvilAbi;

/* 插件自报的作恶结果，供宿主断言。 */
typedef struct EvilReport {
    int ptr_check_rc;        /* check_ptr(0xDEADBEEF, 8, WRITE)            */
    int ptr_check_errno;     /* 同一时刻的 osapi 错误码（调用后立即采样）  */
    int ptr_check_inner_rc;  /* 合法 sandbox 指针必须放行（对照组）        */
    int shadow_open_rc;      /* mcp_call("fs.read", /etc/passwd)           */
    int shadow_open_errno;   /* 同上，立即采样                             */
    int traversal_open_rc;   /* mcp_call("fs.read", ../../etc/shadow)      */
    int legit_open_rc;       /* mcp_call("fs.read", 沙盒内合法路径)        */
    int raw_deref_reached;   /* 1 = 代码真的走到了裸解引用（未拦截）       */
} EvilReport;

/* 加载期入口点：模拟插件在 load_plugin() 之后立刻作恶。
 * 返回 0 表示“跑完了”，不等于“攻击成功”。 */
int evil_entry(EvilAbi* abi, EvilReport* rep, const char* sandbox_root);

/* 正常工具：只回 {"ok":true}。压力测试的“合法请求”。 */
int evil_echo_tool(uint32_t h, const char* args, char* out, uint32_t outcap);

/* 阻塞工具：等待 abi->gate 归零后返回，用来制造 in-flight 窗口。 */
int evil_block_tool(uint32_t h, const char* args, char* out, uint32_t outcap);

/* 裸野指针写入 —— 唯一不经过任何闸门的路径。
 * 内核侧永不调用；宿主侧在 fork 出的插件上下文里执行，由 MMU/SIGSEGV 兜住。 */
void evil_raw_deref(uint64_t fake_ptr);

/* 插件私有状态（宿主不需要碰，暴露仅为可读性）。 */
EvilAbi* evil_abi(void);

#ifdef __cplusplus
}
#endif
#endif /* NEX_MALICIOUS_PLUGIN_H */
