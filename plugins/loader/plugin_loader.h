/* =====================================================================
 *  plugin_loader.h  -  NexOS 内核态插件加载器（freestanding, no stdlib）
 * ---------------------------------------------------------------------
 *  设计目标（与 docs/MCP_PLUGIN_PROTOCOL.md 对应）
 *
 *   1. 一切皆插件：应用只以「整合包（bundle）」分发，包内每个功能都是
 *      一个独立 PluginManifest。加载器不认识“应用”，只认识插件。
 *   2. 单进程、零 IPC：所有插件与宿主同进程，通过**函数指针**调用，
 *      没有消息队列、没有共享内存段、没有 socketpair。
 *      隔离靠“能力位 + 沙盒根 + 句柄代际”三件套实现，而不是靠进程边界。
 *   3. 能力最小化：manifest 申请的位只是“请求”，真正授予的位由
 *      load_plugin() 决定（未签名 -> 强制摘掉 CAP_FS_WRITE）。
 *   4. 禁止标准库：本文件只依赖 <stdint.h> 的定宽整数；内存/字符串
 *      全部由 memset_c / strcpy_c / strcmp_c 等手写实现提供。
 *
 *  句柄编码（O(1) 查找 + 防悬空）
 *  -------------------------------------------------------------------
 *      handle = (generation << 8) | (slot_index + 1)
 *
 *      bit 0..7   : 槽位号 + 1（0 保留为“无效句柄”）
 *      bit 8..31  : 该槽位的代际（每次分配 +1，24 位回绕时跳过 0）
 *
 *  查找只需一次数组下标 + 一次 generation 比较，O(1)。
 *  卸载后旧句柄的 generation 与槽位不再匹配，立即返回 PL_ERR_STALE，
 *  因此“卸载后继续用旧句柄”不可能命中新插件（ABA 问题被代际封死）。
 *
 *  容量：PL_MAX_PLUGINS = 128 个并发插件。槽位用完后 load_plugin()
 *  返回 PL_ERR_NOSPACE，绝不覆盖已加载插件。
 * ===================================================================== */
#ifndef NEX_PLUGIN_LOADER_H
#define NEX_PLUGIN_LOADER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------- 容量 */
#define PL_MAX_PLUGINS   128
#define PL_MAX_ID         32
#define PL_MAX_ROOT       96
#define PL_MAX_PATH      256
#define PL_MAX_TOOL       48
#define PL_MAX_HOST      128
#define PL_MAX_ARGS     2048
#define PL_MAX_OUT      4096
#define PL_TMP_BUF      2048

/* ------------------------------------------------------------ 能力位 */
/* manifest 里声明的是“申请”，PluginContext.caps 是“实际授予”。       */
#define CAP_NONE      0x00000000u
#define CAP_FS_READ   0x00000001u   /* 读取沙盒内文件                      */
#define CAP_FS_WRITE  0x00000002u   /* 写入/删除沙盒内文件（未签名必被摘） */
#define CAP_NET       0x00000004u   /* 发起出站 HTTP(S)（域名白名单）      */
#define CAP_PROC      0x00000008u   /* 读取进程/线程列表                  */
#define CAP_UI        0x00000010u   /* 修改自己的窗口（ui_layout）         */
#define CAP_AI        0x00000020u   /* 调用本机 AI 引擎                    */
#define CAP_DEV       0x00000040u   /* 设备 ioctl（串口/音频/GPIO）        */
#define CAP_SYSINFO   0x00000080u   /* CPU / 内存 / 负载 只读快照          */
#define CAP_IPC       0x00000100u   /* 保留：单进程零 IPC，永远不授予      */
#define CAP_KNOWN     0x000001FFu
#define CAP_ALL       0x7FFFFFFFu

/* -------------------------------------------------------------- 错误码 */
#define PL_OK             0
#define PL_ERR_NOSPACE   (-1)   /* 128 个槽位已满                      */
#define PL_ERR_BADARG    (-2)   /* 空指针 / 非法 id / 缺 path          */
#define PL_ERR_NOENT     (-3)   /* 插件或工具不存在                    */
#define PL_ERR_STALE     (-4)   /* 句柄已悬空（卸载过 / 从未分配）     */
#define PL_ERR_DENIED    (-5)   /* 能力位不足                          */
#define PL_ERR_OVERFLOW  (-6)   /* 输出缓冲区不足                      */
#define PL_ERR_CAP       (-7)   /* manifest 申请了未知/非法能力组合    */
#define PL_ERR_SANDBOX   (-8)   /* 路径越界（bnd_prefix_hit 拒绝）     */
#define PL_ERR_STATE     (-9)   /* 插件正忙 / ABI 不匹配               */
#define PL_ERR_NET       (-10)  /* 目标域名不在 allow_hosts 内         */

/* ------------------------------------------------------------ 前置声明 */
struct PluginManifest;
struct PluginContext;

/* 入口：插件被 load_plugin() 接受后调用一次（可为 NULL，纯 MCP 插件）。 */
typedef int (*pl_entry_fn)(uint32_t h, const char* args,
                           char* out, int outcap);

/* MCP 工具派发：tool 是 "fs.write" / "sys.info" 这类点分名。
 * 返回值 0 表示成功，负值走 MCP error 封装。实现把 JSON 写进 out。 */
typedef int (*pl_call_fn)(uint32_t h, const char* tool, const char* args,
                          char* out, int outcap);

/* 宿主文件钩子。loader 只做策略校验，真正落盘由内核 VFS 提供。
 * 未注册时，fs.* 调用返回 PL_ERR_NOENT。 */
typedef int (*pl_fs_read_fn)(uint32_t h, const char* path,
                             uint8_t* buf, uint32_t cap, uint32_t* out_len);
typedef int (*pl_fs_write_fn)(uint32_t h, const char* path,
                              const uint8_t* data, uint32_t len);

/* 出站网络钩子。host 已通过白名单校验，实现负责 DNS/连接/超时。 */
typedef int (*pl_net_fn)(uint32_t h, const char* method, const char* host,
                         const char* path, const char* body,
                         char* out, int outcap);

/* -------------------------------------------------------------- 清单 */
/* 注意：所有字符串都是**借用**的，必须在插件生命周期内保持有效。
 * bundle 安装器把 manifest.json 解析到内核堆上，卸载时统一释放。 */
typedef struct PluginManifest {
    const char*   id;             /* ^[a-z][a-z0-9._-]{1,31}$            */
    const char*   name;           /* 显示名（UTF-8）                    */
    const char*   version;        /* "1.2.3"                            */
    const char*   sandbox_root;   /* 请求的根；未签名时被忽略           */
    uint32_t      capabilities;   /* CAP_* 位或                       */
    const char*   allow_hosts;    /* 逗号分隔域名；NULL = 禁止联网      */
    const char*   ui_layout;      /* 内联 JSON 或 "ui/layout.json"      */
    pl_entry_fn   entry_point;    /* 可为 NULL                          */
    pl_call_fn    call_point;     /* 可为 NULL（由托管侧派发）          */
    uint32_t      abi_version;    /* 0 视为 1                           */
} PluginManifest;

/* -------------------------------------------------------------- 上下文 */
typedef struct PluginContext {
    uint32_t            h_t;          /* 分配的句柄（0 = 无效）        */
    uint32_t            caps;         /* 实际授予的能力位              */
    uint32_t            abi_version;  /* 协商后的 ABI                  */
    uint32_t            busy;         /* >0 时禁止卸载                 */
    uint32_t            call_count;   /* 诊断计数                      */
    uint8_t             is_signed;    /* 1 = 签名已验证                */
    uint8_t             state;        /* 0 = free, 1 = loading, 2 = live*/
    char                id[PL_MAX_ID];
    char                sandbox_root[PL_MAX_ROOT];
    const PluginManifest* manifest;   /* 借用，生命周期由安装器保证    */
} PluginContext;

/* ------------------------------------------------------------ 生命周期 */
void           pl_init(void);                 /* 清空注册表，可重复调用     */
int            pl_load_plugin(const PluginManifest* m, int is_signed,
                              uint32_t* out_handle);
int            pl_unload(uint32_t h);
int            pl_count(void);                /* 当前 live 插件数           */

/* O(1) 查找。返回的指针在下一次 pl_unload() 前有效。 */
PluginContext* pl_lookup(uint32_t h);
int            pl_list(uint32_t* out, int cap);  /* 导出全部句柄，返回个数 */

/* -------------------------------------------------------------- MCP */
/* 派发一次 tools/call。args_json 形如 {"path":"...","data":"..."}。
 * 成功时 out 写 JSON-RPC 2.0 result，失败时写 JSON-RPC 2.0 error。 */
int mcp_dispatch(uint32_t h, const char* tool, const char* args_json,
                 char* out, int outcap);

/* 路径边界检查：root 必须是绝对路径，path 解析后必须落在 root 之内。
 * 任何 ".."、"\\"、控制字符都直接拒绝（不尝试“修正”）。 */
int bnd_prefix_hit(const char* root, const char* path);

/* 域名白名单检查（大小写不敏感，支持 *.example.com）。 */
int pl_check_host(const PluginContext* ctx, const char* host);

/* -------------------------------------------------------------- 宿主钩子 */
void pl_set_fs_hooks(pl_fs_read_fn rd, pl_fs_write_fn wr);
void pl_set_net_hook(pl_net_fn fn);

/* -------------------------------------------------------------- 自测 */
/* 打印两条强制用例并返回失败数（0 = 全过）：
 *   1) 未签名插件越权被拦截
 *   2) 卸载后旧句柄悬空报错
 */
int run_selftest(void);

const char* pl_strerror(int code);

/* ---------------------------------------------------- 手写底层原语 */
/* 名字带 _c 是为了和内核已有的 my_* / pm_* 系列区分，绝不与 libc 冲突。 */
void*      memset_c(void* dst, int v, uint32_t n);
char*      strcpy_c(char* dst, const char* src);          /* 无边界，慎用 */
char*      strncpy_c(char* dst, const char* src, uint32_t cap);
int        strcmp_c(const char* a, const char* b);
int        str_ieq_c(const char* a, const char* b);
uint32_t   strlen_c(const char* s);
int        str_starts_c(const char* s, const char* prefix);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* NEX_PLUGIN_LOADER_H */
