/* =====================================================================
 *  plugin_core.h  -  NexOS 微内核级插件核心（freestanding, no stdlib）
 * ---------------------------------------------------------------------
 *  这是插件体系的“深水区”：在 loader 的策略层之下，再加一层无锁并发、
 *  指针校验、影子页表、哈希路由与生命周期管控。
 *
 *  铁律
 *  -------------------------------------------------------------------
 *   1. 只包含 <stdint.h>。没有 <string> <vector> <map> <mutex> <thread>，
 *      也没有 malloc/free：所有表都是静态 .bss，槽位用位图 O(1) 分配。
 *   2. 任何错误路径都必须调用 osapi_set_last_error()，返回负的 PC_ERR_*。
 *   3. 句柄查找 = 数组下标 + 代际比较，O(1)；工具路由 = FNV-1a +
 *      开放寻址线性探测，平均 O(1)，删除用 Knuth 回移，不留墓碑。
 *   4. 插件回调永远在锁外执行；锁内只做查表、计数、拷贝。
 *   5. 未签名插件的 PagePermission 被硬重置为唯一一段
 *      [sandbox_base, sandbox_base+sandbox_size) 读写页，manifest 里
 *      申请的额外页和 EXEC 权限一律丢弃。
 *
 *  句柄编码
 *  -------------------------------------------------------------------
 *      handle = (generation << 8) | (slot_index + 1)
 *      bit 0..7  : 槽位 + 1（0 是无效句柄）
 *      bit 8..31 : 24 位代际
 *
 *  卸载时 Gen++，加载时再 Gen++；旧句柄的 gen 与槽位不匹配，
 *  resolve_locked() 直接返回 NULL，dispatch 返回 PC_ERR_STALE。
 *
 *  容量
 *  -------------------------------------------------------------------
 *      PC_MAX_PLUGINS   128      并发插件
 *      PC_ROUTER_SLOTS  512      MCP 工具路由槽（上限约 75% 负载）
 *      PC_MAX_INFLIGHT   10      单插件默认并发配额（可调，硬上限 32）
 *
 *  注意：本模块静态表实测 .bss = 100488 B（约 98 KiB），.text = 31520 B
 *  （约 31 KiB），默认不链入 kernel64。启用前必须跑
 *  tools/verify/check_k64_fit.sh（见 docs/RULES.md）。
 * ===================================================================== */
#ifndef NEX_PLUGIN_CORE_H
#define NEX_PLUGIN_CORE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------- 容量 */
#define PC_MAX_PLUGINS        128
#define PC_MAX_ID              32
#define PC_MAX_ROOT            96
#define PC_MAX_PATH           256
#define PC_MAX_TOOL            48
#define PC_MAX_JSON          2048
#define PC_MAX_JSON_KEY        64
#define PC_JSON_MAX_DEPTH      16
#define PC_MAX_OUT           4096
#define PC_MAX_PAGES            4
#define PC_MAX_INFLIGHT        10
#define PC_HARD_INFLIGHT       32
#define PC_ROUTER_SLOTS       512
#define PC_ROUTER_MASK        (PC_ROUTER_SLOTS - 1)
#define PC_ROUTER_MAX_LOAD_NUM 3   /* 负载 <= 3/4 */
#define PC_ROUTER_MAX_LOAD_DEN 4
#define PC_MIN_SANDBOX       4096u
#define PC_MAX_SANDBOX       (1024u * 1024u)

/* -------------------------------------------------------------- 错误码 */
#define PC_OK              0
#define PC_ERR_NOSPACE    (-1)   /* 槽位/路由表满（触发 OOM 熔断）     */
#define PC_ERR_BADARG     (-2)   /* 空指针 / 非法 id / 非法长度        */
#define PC_ERR_NOENT      (-3)   /* 插件或工具不存在                   */
#define PC_ERR_STALE      (-4)   /* 句柄已悬空（卸载过 / 从未分配）    */
#define PC_ERR_DENIED     (-5)   /* 能力位/指针/域名被拒绝             */
#define PC_ERR_OVERFLOW   (-6)   /* 缓冲区不足（已安全截断）           */
#define PC_ERR_CAP        (-7)   /* 清单申请了未知/非法能力            */
#define PC_ERR_SANDBOX    (-8)   /* 路径越界                           */
#define PC_ERR_STATE      (-9)   /* 状态不允许（未初始化等）           */
#define PC_ERR_BUSY      (-10)   /* 并发配额耗尽 / 卸载时仍有调用      */
#define PC_ERR_NOMEM     (-11)   /* 保留：静态池不应发生               */
#define PC_ERR_QUOTA     (-12)   /* 配额配置非法                       */

/* ------------------------------------------------------------ 能力位 */
#define PC_CAP_NONE       0x00000000u
#define PC_CAP_FS_READ    0x00000001u
#define PC_CAP_FS_WRITE   0x00000002u   /* 未签名必被摘除                */
#define PC_CAP_NET        0x00000004u
#define PC_CAP_PROC       0x00000008u
#define PC_CAP_UI         0x00000010u
#define PC_CAP_AI         0x00000020u
#define PC_CAP_DEV        0x00000040u
#define PC_CAP_SYSINFO    0x00000080u
#define PC_CAP_IPC        0x00000100u   /* 单进程零 IPC，永不授予        */
#define PC_CAP_KNOWN      0x000001FFu

/* ---------------------------------------------------------- 页权限位 */
#define PC_PROT_NONE      0x0u
#define PC_PROT_READ      0x1u
#define PC_PROT_WRITE     0x2u
#define PC_PROT_EXEC      0x4u
#define PC_PROT_RW        (PC_PROT_READ | PC_PROT_WRITE)

/* ------------------------------------------------------------ 工具标志 */
#define PC_TOOL_PATH_SANDBOX  0x01u  /* 调用前必须校验 args.path      */
#define PC_TOOL_USER_PTR      0x02u  /* 调用前必须校验 args.ptr/len   */

/* ---------------------------------------------------------- 用户指针域 */
#ifndef PC_USER_PTR_MIN
#  define PC_USER_PTR_MIN  0x0000000000010000ULL
#endif
#ifndef PC_USER_PTR_MAX
#  define PC_USER_PTR_MAX  0x00007FFFFFFFFFFFULL
#endif

/* 前置声明：is_valid_user_ptr() 需要它，但完整定义在模块四。 */
typedef struct PcPlugin PcPlugin;

/* =====================================================================
 *  模块一：自旋锁
 * ===================================================================== */
typedef struct spinlock_t {
    volatile uint32_t lock;      /* 0 = free, 1 = held                  */
} spinlock_t;

void spin_init(spinlock_t* l);
int  spin_trylock(spinlock_t* l);          /* 1 = 拿到，0 = 忙          */
void spin_lock(spinlock_t* l);
void spin_unlock(spinlock_t* l);

/* 内核态下先 cli 再自旋，避免中断处理程序递归抢同一把锁。
 * 宿主自测返回 0，恢复时什么也不做（绝不能 cli 用户态）。 */
uint32_t spin_lock_irqsave(spinlock_t* l);
void     spin_unlock_irqrestore(spinlock_t* l, uint32_t flags);

/* =====================================================================
 *  模块一：安全内存/字符串原语
 * ---------------------------------------------------------------------
 *  safe_* 只负责“空指针 + 长度/边界”，不猜地址是否可访问。
 *  来自插件（用户侧）的指针必须先过 is_valid_user_ptr()，再用
 *  safe_copy_from_user()。这样 Page Fault 在进入拷贝循环之前就被挡住。
 * ===================================================================== */
void* safe_memset(void* dst, int v, uint32_t n);

/* 0 = 成功；PC_ERR_BADARG = 空指针/长度越界（此时不写任何字节）。
 * 要求 dst_cap >= n，绝不部分拷贝（要么全做，要么不做）。 */
int safe_memcpy(void* dst, uint32_t dst_cap, const void* src, uint32_t n);

/* 0 = 成功；PC_ERR_BADARG = 空指针/容量 0；
 * PC_ERR_OVERFLOW = 源串过长，已截断并 NUL 结尾。 */
int safe_strcpy(char* dst, uint32_t dst_cap, const char* src);
int safe_strncpy(char* dst, uint32_t dst_cap, const char* src, uint32_t n);

/* 最多读 cap 字节；没有 NUL 就返回 cap（绝不越界读）。 */
uint32_t safe_strlen(const char* s, uint32_t cap);
int      safe_strcmp(const char* a, const char* b);   /* 空指针安全        */
int      safe_strncmp(const char* a, const char* b, uint32_t n);
int      safe_streq(const char* a, const char* b);    /* 1 = 相等          */

/* =====================================================================
 *  模块二：MMU/页表级隔离影子
 * ===================================================================== */
typedef struct PagePermission {
    uint64_t base;       /* 页起始虚地址                                */
    uint64_t size;       /* 字节数                                      */
    uint32_t prot;       /* PC_PROT_*                                   */
    uint32_t owner;      /* 句柄；0 = 内核/共享页                       */
} PagePermission;

/* 校验 ptr..ptr+len-1 是否完全落在该插件已授予的页内，且 prot 允许。
 * 返回 1 = 合法；0 = 拒绝（同时 osapi_set_last_error）。 */
int is_valid_user_ptr(const PcPlugin* p, const void* ptr,
                      uint32_t len, uint32_t prot);
int is_valid_user_ptr_h(uint32_t h, const void* ptr,
                        uint32_t len, uint32_t prot);

/* 先做用户指针校验，再 safe_memcpy。 */
int safe_copy_from_user(const PcPlugin* p, void* dst, uint32_t dst_cap,
                        const void* user_src, uint32_t len);

/* =====================================================================
 *  模块三：FNV-1a + MCP 哈希路由
 * ===================================================================== */
uint32_t fnv1a32(const void* data, uint32_t len);
uint32_t fnv1a32_str(const char* s);

typedef int (*pc_tool_fn)(uint32_t h, const char* args_json,
                          char* out, uint32_t outcap);

typedef struct McpRoute {
    uint32_t  hash;
    uint32_t  handle;
    uint32_t  caps;          /* 调用该工具至少需要的能力位              */
    uint32_t  flags;         /* PC_TOOL_*                               */
    pc_tool_fn fn;
    uint16_t  next;          /* 同一插件的下一条路由（1-based，0=尾）   */
    uint8_t   state;         /* 0=空 1=占用                             */
    uint8_t   pad;
    char      tool[PC_MAX_TOOL];
} McpRoute;

typedef struct McpRouter {
    spinlock_t lock;
    McpRoute   entries[PC_ROUTER_SLOTS];
    uint16_t   heads[PC_MAX_PLUGINS];  /* 每插件路由链头（1-based）      */
    uint32_t   used;
    uint64_t   lookups;
    uint64_t   hits;
    uint64_t   probes;                 /* 累计探测次数，用于诊断         */
    uint32_t   probe_max;
} McpRouter;

void router_init(McpRouter* r);
int  router_register(McpRouter* r, uint32_t h, const char* tool,
                     pc_tool_fn fn, uint32_t caps, uint32_t flags);
int  router_unregister_tool(McpRouter* r, const char* tool);
int  router_unregister_handle(McpRouter* r, uint32_t h);
/* 成功返回 PC_OK 并填 *out；未命中返回 PC_ERR_NOENT。 */
int  router_lookup(McpRouter* r, const char* tool, McpRoute* out);

/* =====================================================================
 *  模块三：一层 JSON 解析（只取顶层 key -> value）
 * ---------------------------------------------------------------------
 *  - 完整校验整个文档结构（嵌套对象/数组只做跳过，不展开）
 *  - 字符串支持 \" \\ \/ \b \f \n \r \t \uXXXX（含代理对）
 *  - \u0000 视为非法，防止路径截断攻击
 *  - 整数支持 int64 溢出检测；小数/指数对 json_get_int64 返回 BADARG
 * ===================================================================== */
int json_get_string(const char* json, const char* key,
                    char* out, uint32_t outcap);
int json_get_int64(const char* json, const char* key, int64_t* out);
int json_get_bool(const char* json, const char* key, int* out);
int json_has_key(const char* json, const char* key);

/* 把字符串转义后写入 out（用于错误响应）。 */
int json_escape_into(char* out, uint32_t outcap, const char* s);

/* =====================================================================
 *  模块四：生命周期
 * ===================================================================== */
typedef struct PcManifest {
    const char*          id;
    const char*          sandbox_root;   /* 仅签名插件可能生效          */
    uint64_t             sandbox_base;   /* 宿主预留给该插件的页基址    */
    uint32_t             sandbox_size;   /* >= PC_MIN_SANDBOX           */
    uint32_t             capabilities;
    uint32_t             quota;          /* 0 = PC_MAX_INFLIGHT         */
    uint32_t             abi_version;    /* 0 = 1                       */
    const PagePermission* extra_pages;   /* 仅签名插件生效              */
    uint32_t             extra_page_count;
} PcManifest;

struct PcPlugin {
    volatile uint32_t state;       /* 0=free 1=live                       */
    uint32_t          gen;         /* 24 位代际                           */
    uint32_t          h;           /* 当前句柄（0 = 无）                  */
    uint32_t          slot;
    uint32_t          caps;
    uint32_t          quota;
    volatile uint32_t inflight;    /* 当前并发调用数                      */
    uint32_t          call_count;
    uint8_t           is_signed;
    char              id[PC_MAX_ID];
    char              sandbox_root[PC_MAX_ROOT];
    uint64_t          sandbox_base;
    uint64_t          sandbox_size;
    PagePermission    pages[PC_MAX_PAGES];
    uint32_t          page_count;
};

void pc_set_emit_hook(void (*fn)(const char* s));

int  pc_core_init(void);                 /* 清空表，可重复调用            */
int  load_plugin(const PcManifest* m, int is_signed, uint32_t* out_h);
int  unload_plugin(uint32_t h);
int  plugin_count(void);

/* O(1) 诊断查询。返回的是内部指针：只在没有并发 unload 时有效。
 * 需要跨锁使用时请用 plugin_snapshot()。 */
PcPlugin* plugin_lookup(uint32_t h);
int       plugin_snapshot(uint32_t h, PcPlugin* out);

/* 注册工具。need_caps 必须被插件实际授予的能力位覆盖。
 * 同名工具全局唯一；重复注册返回 PC_ERR_DENIED。 */
int register_tool(uint32_t h, const char* tool, pc_tool_fn fn,
                  uint32_t need_caps, uint32_t flags);
int unregister_tool(uint32_t h, const char* tool);

/* MCP tools/call 入口：
 *   句柄代际 -> 路由 O(1) -> 能力位 -> 路径沙盒 -> 用户指针 ->
 *   并发配额 -> 锁外调用 -> inflight--。
 * 失败时自动在 out 写 JSON-RPC 2.0 error。 */
int dispatch_mcp(uint32_t h, const char* tool, const char* args_json,
                 char* out, uint32_t outcap);

/* OOM 熔断：128 槽满后置位；即使随后有空槽也拒绝新加载，
 * 直到显式清除。用于把“内存耗尽”变成可观测状态而不是随机失败。 */
int pc_oom_tripped(void);
int pc_clear_oom_fuse(void);

const char* pc_strerror(int code);

/* 模块五：串口自测。返回失败用例数（0 = 全过）。 */
int run_core_selftest(void);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* NEX_PLUGIN_CORE_H */
