# 微内核级插件加载器与 MCP 路由总线（`plugin_core`）

> 位置：`plugins/loader/plugin_core.{h,cpp}`（内核侧，freestanding）
> 自测：`plugins/loader/core_selftest.cpp` + `make plugin-core-selftest`
> 状态：**68 / 68 用例通过**，freestanding 编译 **0 warning**

`plugin_core` 是 `plugin_loader`（策略层：能力位 / 沙盒根 / 清单校验）之下的
**机制层**：自旋锁、安全内存原语、影子页表、FNV-1a 哈希路由、句柄生命周期与
并发配额。两层互不依赖，`pl_*` 与 `load_plugin` / `dispatch_mcp` 可以并存。

---

## 1. 铁律

| 约束 | 落实方式 |
|------|----------|
| 无 STL / 无 libc | 只 `#include <stdint.h>`；表全是静态 `.bss`；字符串/内存手写 |
| 错误必须走内核契约 | 所有失败路径经 `pc_fail(rc, os_err)` → `osapi_set_last_error()` |
| O(1) 查找 | 句柄 = 数组下标 + 代际比较；路由 = FNV-1a + 开放寻址线性探测 |
| 插件崩不了内核 | 回调永远在锁外执行；用户指针先过影子页表再拷贝；并发配额限流 |
| 关中断只在 ring0 | `spin_lock_irqsave()` 在宿主自测下为 no-op（绝不 cli 用户态） |

---

## 2. 五个模块

### 模块一 · 无锁并发与底层防御

```c
typedef struct spinlock_t { volatile uint32_t lock; } spinlock_t;

void     spin_init(spinlock_t*);
int      spin_trylock(spinlock_t*);       /* 1 = 拿到，0 = 忙 */
void     spin_lock(spinlock_t*);
void     spin_unlock(spinlock_t*);
uint32_t spin_lock_irqsave(spinlock_t*);  /* cli + 自旋，返回 EFLAGS */
void     spin_unlock_irqrestore(spinlock_t*, uint32_t flags);
```

- CAS：`__atomic_compare_exchange_n`（x86 内联 `lock cmpxchg`，ARM 内联
  `ldaxr/stlxr`）；无 `__atomic` 的 x86 走手写汇编；MSVC 走 `_InterlockedCompareExchange`。
- 自旋体带 `pause` / `yield`，避免超线程下的流水线惩罚。
- `spin_lock_irqsave()` 只在内核态生效：先 `pushfq/popq/cli`，恢复时**只有原本
  IF=1 才 sti**，不会把中断提前打开。

安全内存/字符串原语（全部处理空指针与截断，绝不部分写入）：

| 函数 | 语义 |
|------|------|
| `safe_memset(dst,v,n)` | `dst==NULL` 直接返回，不写 |
| `safe_memcpy(dst,cap,src,n)` | `n>cap` / 空指针 → `PC_ERR_BADARG`，**要么全拷贝，要么一个字节都不写** |
| `safe_strcpy(dst,cap,src)` | 截断并 NUL 结尾，源串过长 → `PC_ERR_OVERFLOW` |
| `safe_strncpy(dst,cap,src,n)` | 同上，受 `n` 与 `cap` 双约束 |
| `safe_strlen(s,cap)` | 最多读 `cap` 字节，没有 NUL 就返回 `cap`（绝不越界读） |
| `safe_strcmp/strncmp/streq` | 空指针安全比较 |

### 模块二 · MMU/页表级隔离影子

```c
typedef struct PagePermission {
    uint64_t base, size;
    uint32_t prot;    /* PC_PROT_READ | WRITE | EXEC */
    uint32_t owner;   /* 句柄；0 = 内核共享页 */
} PagePermission;

int is_valid_user_ptr(const PcPlugin*, const void* ptr, uint32_t len, uint32_t prot);
int is_valid_user_ptr_h(uint32_t h, const void* ptr, uint32_t len, uint32_t prot);
int safe_copy_from_user(const PcPlugin*, void* dst, uint32_t cap,
                        const void* user_src, uint32_t len);
```

校验顺序（任何一步失败立即拒绝，**不进入拷贝循环**）：

1. `ptr != NULL`、`len != 0`；
2. `prot` 必须是合法位组合；
3. `end = start + len - 1`，`end < start` 视为回绕 → 拒绝；
4. 必须落在用户地址域 `[0x10000, 0x7FFF_FFFF_FFFF]`；
5. 必须被**该插件自己的**某一条 `PagePermission` 完整包含，且 `prot` 覆盖所需权限位。

未签名插件的影子页表在 `load_plugin()` 里被**硬重置**为唯一一段：

```
pages[0] = { base = manifest.sandbox_base, size = manifest.sandbox_size,
             prot = PC_PROT_RW, owner = handle }
page_count = 1
```

manifest 里申请的 `extra_pages` 与 `PC_PROT_EXEC` **一律丢弃**；沙盒根同样被
强制成 `/sandbox/<id>`。签名插件才可以保留额外页（最多 `PC_MAX_PAGES-1` 条，
含 EXEC），且 `sandbox_root` 仍必须位于 `/sandbox/` 之下。

### 模块三 · FNV-1a 哈希路由 + 一层 JSON

```c
#define PC_ROUTER_SLOTS 512
typedef struct McpRoute {
    uint32_t hash, handle, caps, flags;
    pc_tool_fn fn;
    uint16_t next;      /* 同插件路由链（1-based，0 = 尾） */
    uint8_t  state;     /* 0 = 空，1 = 占用 */
    char     tool[PC_MAX_TOOL];
} McpRoute;

typedef struct McpRouter {
    spinlock_t lock;
    McpRoute   entries[PC_ROUTER_SLOTS];
    uint16_t   heads[PC_MAX_PLUGINS];
    uint32_t   used;
    uint64_t   lookups, hits, probes;
    uint32_t   probe_max;
} McpRouter;
```

- 哈希：FNV-1a 32 位（`offset 2166136261` / `prime 16777619`）。
- 冲突：开放寻址**线性探测**；负载上限 3/4（384 条），超限直接 `PC_ERR_NOSPACE`。
- 删除：**Knuth 回移**，不留墓碑 —— 空槽永远是探测链终点，所以查找在第一个空槽
  就可以停。回移条件按环状区间判断：

  ```c
  if (hole <= j)  can_move = (home <= hole || home > j);
  else            can_move = (home <= hole && home > j);
  ```

  不可搬的条目**跳过继续向后扫描**（不能遇到 `home == j` 就 break，否则会留下
  一个挡住后续同簇条目的空洞 —— 这正是自测 `churn.t121` 曾经命不中的原因）。
- 每插件维护一条单向路由链 `heads[slot]`，`unload_plugin()` 按句柄一次性摘除整条链，
  搬移槽位时同步 `pc_chain_repoint()` 修正链表下标与 `heads`。

JSON 只解析**顶层 key → value**，但会完整校验整份文档（嵌套对象/数组只跳过）：

- 字符串转义 `\" \\ \/ \b \f \n \r \t \uXXXX`，支持 UTF-8 代理对；
- `\u0000` 直接判非法（否则 C 字符串截断可以绕过路径检查）；
- 数字严格按 JSON 文法，`json_get_int64` 做溢出检测并正确处理 `INT64_MIN`；
- 键比较按**原始字节**：`"\u0070ath"` 不会被当成 `path`，攻击者无法用转义键冒充。

### 模块四 · 生命周期

```c
int load_plugin(const PcManifest* m, int is_signed, uint32_t* out_h);
int dispatch_mcp(uint32_t h, const char* tool, const char* args_json,
                 char* out, uint32_t outcap);
int unload_plugin(uint32_t h);
```

`load_plugin` 顺序：清单校验 → 能力位裁剪（未签名摘 `PC_CAP_FS_WRITE`，`PC_CAP_IPC`
无条件拒绝）→ 沙盒根定型 → 取锁 → OOM 熔断检查 → 位图分配槽位（`ctz` O(1)）→
`Gen++` → 填上下文与影子页表。

句柄编码：

```
handle = (generation << 8) | (slot_index + 1)      /* 低 8 位 0 = 无效句柄 */
```

`dispatch_mcp` 的完整防线（任何一步失败都写 JSON-RPC 2.0 error 并置 last_error）：

1. **代际校验**：`pc_resolve_locked()` 一次下标 + 一次 gen 比较，O(1)。
2. **并发配额**：`inflight >= quota` → `PC_ERR_BUSY`（默认 10，硬上限 32）。
   通过后 `inflight++`，此后该插件不可能被 `unload`（见下）。
3. **路由查找**：FNV-1a + 线性探测，`route.handle != h` → `PC_ERR_DENIED`
   （防止 A 插件调用 B 插件的工具）。
4. **能力位**：`route.caps & ~plugin.caps` 非零 → 拒绝。
5. **路径沙盒**（`PC_TOOL_PATH_SANDBOX`）：解析 `args.path` → 规范化 → 前缀比对。
   `/etc/passwd`、`../../etc/shadow`、`/sandbox/audit2`（兄弟前缀）、反斜杠
   全部拒绝。
6. **用户指针**（`PC_TOOL_USER_PTR`）：解析 `args.ptr/len` → `is_valid_user_ptr()`
   → 拒绝伪造地址。
7. **锁外调用**：`route.fn(...)`；插件回调里可以安全地再次调 `dispatch_mcp`
   （重入会被配额挡下）。
8. `inflight--`，并把负返回码映射成 JSON-RPC 错误。

`unload_plugin` 顺序：取锁 → 解析句柄 → `inflight > 0` 返回 `PC_ERR_BUSY`
（不强行释放，避免悬空回调）→ **先注销整条路由链**（反过来会留下“插件已死、
路由仍在”的窗口）→ 清上下文 → `Gen++` → 释放位图槽位。

OOM 熔断：128 槽满后置 `g_oom_tripped`，**即使随后有空槽也拒绝新加载**，直到
`pc_clear_oom_fuse()`。把「内存耗尽」变成可观测状态，而不是随机失败。

### 模块五 · 内核防线自测

`run_core_selftest()` 直接向串口（或宿主 hook）打印，返回失败用例数。

```
===== NexOS plugin_core selftest =====

--- 场景 A：恶意插件传入伪造指针 ---
[SELFTEST] PASS  unsigned plugin loads
[SELFTEST] PASS  unsigned plugin: CAP_FS_WRITE stripped
[SELFTEST] PASS  unsigned plugin: root forced to /sandbox/audit
[SELFTEST] PASS  unsigned plugin: exactly one RW sandbox page
[SELFTEST] PASS  forged 0xDEADBEEF00000000 rejected
[SELFTEST] PASS  pointer crossing sandbox end rejected
[SELFTEST] PASS  wrapping pointer (end < start) rejected
[SELFTEST] 场景 A: 伪造指针被 is_valid_user_ptr 拦截: 0xdeadbeef00000000 -> errno=4

--- 场景 B：MCP 传入 {"path":"/etc/passwd"} ---
[SELFTEST] PASS  dispatch /etc/passwd -> PC_ERR_SANDBOX + JSON error
[SELFTEST] 场景 B: /etc/passwd 被路径校验拦截:
  {"jsonrpc":"2.0","id":null,"error":{"code":-32001,
   "message":"path escapes sandbox","data":{"errno":-8,"tool":"fs.read"}}}
[SELFTEST] PASS  dispatch traversal ../../etc/shadow denied
[SELFTEST] PASS  dispatch sibling prefix /sandbox/audit2 denied

--- 场景 C：多线程并发 load_plugin ---
[SELFTEST] PASS  concurrent loads all succeeded
[SELFTEST] PASS  no duplicate handle / slot
[SELFTEST] 场景 C: 8 线程 / 64 插件，重复槽位=0

--- 场景 D：卸载后旧句柄失效 ---
[SELFTEST] PASS  old handle dispatch -> PC_ERR_STALE
[SELFTEST] PASS  route removed by unload
[SELFTEST] PASS  reused slot gets a different generation
[SELFTEST] 场景 D: 旧句柄 0x301 -> -4 (stale), 新句柄 0x501

===== plugin_core selftest: 68 passed, 0 failed =====
```

除 A–D 之外还覆盖：签名插件额外页/EXEC、200 条路由删一半后的回移删除、
`\u0000`/转义键/`INT64_MIN` 的 JSON 对抗、并发配额重入、OOM 熔断锁存与解除。

---

## 3. 容量与体积

| 项 | 值 |
|----|-----|
| 并发插件 | `PC_MAX_PLUGINS = 128`（32 位位图，`ctz` O(1) 分配） |
| 路由槽 | `PC_ROUTER_SLOTS = 512`（负载上限 384） |
| 单插件并发 | `PC_MAX_INFLIGHT = 10`（manifest 可调，硬上限 32） |
| 沙盒大小 | `[4 KiB, 1 MiB]` |
| `.text` | **31520 B**（约 31 KiB） |
| `.bss` | **100488 B**（约 98 KiB） |

## 4. 如何启用 / 如何自测

```bash
# 宿主自测（不需要启动 VM，Linux/macOS/Windows+MSYS 都可）
make plugin-core-selftest        # 68 passed, 0 failed

# freestanding 编译（与 kernel64 同一套 flags）
make build/plugin_core64.o       # 0 warning

# 链入 64 位内核前，必须先过容量门禁
bash tools/verify/check_k64_fit.sh
make ... K64_PLUGIN_CORE=1
```

`K64_PLUGIN_CORE` 默认为 `0`：`.bss` 约 98 KiB，是否放得下必须由
`tools/verify/check_k64_fit.sh` 现场判断（见 `docs/RULES.md`）。

在别的宿主（例如 Rust 运行时、C# WinHost）里复用时，只需要：

1. 提供 `osapi_set_last_error()` / `osapi_get_last_error()`（内核由 `osapi.cpp` 提供）；
2. 可选：`pc_set_emit_hook(fn)` 接管串口输出；
3. 可选：提供强符号 `serial_puts`（本模块内部声明为 `weak`，没有也能链接）。

## 5. 威胁模型边界（明确不承诺的部分）

| 威胁 | 处理 |
|------|------|
| 伪造/越界/回绕的用户指针 | `is_valid_user_ptr()` 在拷贝前拒绝 |
| 跨插件句柄调用 | 路由 `handle` 必须等于调用者句柄 |
| 未签名插件申请写权限/额外页 | 加载时硬裁剪，manifest 无效 |
| 路径穿越 / 兄弟前缀 / 反斜杠 | 路径规范化 + 前缀边界 |
| MCP 参数里塞 `\u0000` 截断 | JSON 层直接拒绝 |
| 插件回调里递归派发 | 并发配额（inflight） |
| 槽位复用导致旧句柄命中新插件 | 24 位代际，卸载与复用都 `Gen++` |
| 内存耗尽 | OOM 熔断锁存，可观测可清除 |
| **插件死循环** | 本层**不做抢占**：靠宿主侧超时 abort（`src/plugin_runtime.rs`）或看门狗；内核态同步调用需自行配合定时器 |
| **插件直接写内核地址** | 同地址空间，靠编译期/加载期不授予内核地址页来约束；真正的强隔离需要 ring3 或 VMM 分页（未做） |
| **栈溢出 / 非法指令** | 由 `nexos_compat.cpp` 的 fault 钩子（IST 故障栈 + longjmp）兜底 |

## 6. 与 `plugin_loader` 的分工

| 层 | 文件 | 关注点 |
|----|------|--------|
| 策略层 | `plugin_loader.{h,cpp}`（`pl_*`） | 清单语义、能力位、`bnd_prefix_hit`、33 项策略自测 |
| 机制层 | `plugin_core.{h,cpp}` | 自旋锁、影子页表、哈希路由、句柄代际、并发配额、OOM 熔断 |

两层可以直接串联：`pl_load_plugin()` 校验通过后，用其规范化的 manifest 调
`load_plugin()` 建立机制层上下文；MCP 请求统一从 `dispatch_mcp()` 进入。
