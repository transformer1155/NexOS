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

---

## 7. 内核集成（阶段二：C++ 到 C# 的跨界网关）

```text
  C# (MiniCLR)                     .attic64/kernel64.cpp        plugins/loader/
  ────────────                     ─────────────────────        ───────────────
  NexOS.Plugin.Dispatch(h,t,a)  →  clr_register_icall(      →   osapi_ic_plugin_dispatch()
                                     "NexOS.Plugin::Dispatch")        │
                                                                      ├ clr_str(a[1]) / clr_str(a[2])
                                                                      ├ dispatch_mcp(h,t,a,out)
                                                                      │     ├ 句柄代际校验
                                                                      │     ├ 能力位 & 路由归属
                                                                      │     ├ 路径沙盒 / 用户指针闸门
                                                                      │     └ 锁外执行插件回调
                                                                      └ clr_new_str(out) → 托管字符串
```

**文件**

| 文件 | 角色 |
|------|------|
| `plugins/loader/plugin_bridge.{h,cpp}` | 内核接线层：开机自检 + `osapi_ic_plugin_dispatch` / `osapi_ic_plugin_count` / `osapi_ic_plugin_alive` |
| `plugins/loader/malicious_plugin.{h,cpp}` | **真实**的未签名恶意插件 TU（宿主测试与内核 demo 共用同一份源码） |
| `plugins/loader/breach_test.cpp` | 宿主破坏性测试（阶段三 + 阶段四），`make plugin-core-breach` |
| `csharp/NexOS.Core/Plugin.cs` | 托管侧 `NexOS.Plugin.Bus`（`#if !WINHOST` 时是 InternalCall 声明） |

**icall 契约**（`NexOS.Plugin::Dispatch`）

| 参数 | 含义 |
|------|------|
| `a[0]` | 插件句柄（内核 load 时分配，C# 不可伪造语义） |
| `a[1]` | 工具名（托管字符串） |
| `a[2]` | 参数 JSON（托管字符串），空则按 `{}` 处理 |
| 返回值 | 托管字符串：成功 = 工具 JSON；失败 = JSON-RPC 2.0 error 信封 |

失败时同时 `osapi_set_last_error()`，映射关系与 `dispatch_mcp` 一致
（`PC_ERR_SANDBOX`→`ERR_PERMDENIED`、`PC_ERR_STALE`→`ERR_STALE`、
`PC_ERR_BUSY`→`ERR_BUSY`…）。零拷贝路径可读 `pc_bridge_result_buffer()`
拿同一块 `PC_MAX_OUT` 字节缓冲。

**接线方式：weak 符号。** `kernel64.cpp` 以 `__attribute__((weak))` 引用
`pc_bridge_boot` / `osapi_ic_plugin_dispatch`，模块没链进来时地址为 0 直接跳过。
这样 `K64_PLUGIN_CORE=0/1` 都不需要额外的 `-D` 宏，也不会把未定义符号绑成
0 后调用（triple fault 的经典成因，见 `tools/check_k64_undefined.sh`）——
weak 未定义符号不进 `undefined reference`，`check-k64` 报告保持 `0 (clean)`。

**容量实测**（`tools/verify/check_k64_fit.sh`）

| 配置 | `.bss` | `__bss_end` | `.text` |
|------|-------:|------------:|--------:|
| `K64_PLUGIN_CORE=0` | 3707552 | `0x5c92a0` | 890227 |
| `K64_PLUGIN_CORE=1` | 3801104 | `0x5e0010` | 911347 |
| 增量 | **+93552** | +94064 | +21120 |

链接期上限 `0x600000`（`ASSERT(__bss_end <= 0x600000)`）→ 余量 **130544 B**（127 KiB）。
`kernel64.bin` = 1203600 B（限 2097152 B，spare 1745 sectors）。
明细：`plugin_core64.o` 78176 + `plugin_bridge64.o` 12296 + `malicious64.o` 8 + 夹逼。

三个目标文件在 `-ffreestanding` 下编译均 **0 warning**（`plugin_core.cpp` 的
`pc_emit()` 在无测试脚手架时会被判 `-Wunused-function`，标 `__attribute__((used))`
解决 —— 注意 `maybe_unused` 对函数是 no-op 且会被 GCC 警告 `attribute directive ignored`）。

阶段一还顺手省了 22 KiB：`plugin_core.cpp` 模块五（内核防线自测）带编译门禁
`#if defined(NEX_PC_HOST_TEST) || defined(PC_ENABLE_CORE_SELFTEST)`，内核镜像
只链 `plugin_bridge` 的精简版自检（4 KiB 沙盒页）。首次启用时该模块曾把
`__bss_end` 顶到 `0x600010` —— 比 `ASSERT` 上限还高 16 字节，链接直接失败；
这就是“先跑容量门禁再打开开关”的教科书案例。

---

## 8. 真实恶意插件防穿透（阶段三）

`malicious_plugin.cpp` 不是伪代码：它被真的编译成独立目标文件，
`nm -u build/malicious64.o` **输出为空** —— 没有 `fopen`、没有 `malloc`、
没有 `printf`。它能看到的全部世界就是一行 `EvilAbi` 函数指针表。

> 这就是“插件调 `fopen("/etc/shadow")` 会被拦下”的真正原因：不是我们给
> `fopen` 打了补丁，而是**插件里根本没有 `fopen`**。任何文件访问都必须走
> 宿主的 `mcp_call()`，也就是内核的 `dispatch_mcp()`。控制点等于必经之路，
> 这是微内核式能力模型成立的前提。

`make plugin-core-breach`（宿主，16 线程 + fork 故障隔离）**40 / 40 通过**：

> 两个宿主侧的“坑”值得记下来：① 指针参数必须按 **64 位**打印 —— 宿主是
> PIE，沙盒页常落在 `0x5555_xxxx_xxxx`，用 `%u` 截断成 32 位后闸门会**正确地**
> 拒绝那个错地址，测试就会误判成闸门失效；② errno 必须在拒绝的**当下**采样
> （`EvilAbi.last_errno`），否则后面那次 `mcp_call` 会把错误通道改写，
> 断言读到的是别人的 errno（生产代码里的等价铁律：用完即弃的错误码要立刻取值）。

| 断言 | 结果 |
|------|------|
| `load_plugin(manifest.sandbox_root="/etc", is_signed=0)` | OK，且 `sandbox_root` 被强制改写成 `/sandbox/evil.host` |
| 未签名 → `CAP_FS_WRITE` | 加载期硬裁剪 |
| 影子页表 | 恰好一个 RW 页、无 EXEC、基址 == 宿主给的沙盒页 |
| `is_valid_user_ptr(0xDEADBEEF, 8, WRITE)` | `PC_ERR_DENIED`，errno = `ERR_PERMDENIED` |
| 插件的裸解引用 | **从未执行**（闸门先命中） |
| 对照：沙盒内合法指针 | 放行（证明不是“一律拒绝”） |
| `fs.read /etc/passwd`、`/etc/shadow` | `PC_ERR_SANDBOX`（→ `ERR_PERMDENIED`） |
| `fs.read /sandbox/evil.host/../../etc/shadow` | `PC_ERR_SANDBOX`（穿越被规范化拦下） |
| 对照：`/sandbox/evil.host/notes.txt` | `PC_OK` |
| 绕过闸门的裸野指针 | 在 fork 出的插件上下文里真的 `SIGSEGV`，**宿主进程完好** |

红线日志（串口 / stdout 同款格式）：

```text
[REDLINE] A: forged pointer 0xdeadbeef  is_valid_user_ptr  addr=0xdeadbeef  rc=-5  errno=4  -> blocked, kernel alive
[REDLINE] B: open /etc/shadow  sandbox prefix  rc=-8  errno=4  -> blocked, kernel alive
[REDLINE] raw write to 0xdeadbeef faulted the plugin process, host alive
```

### 内核侧等价自检（开机跑一次）

`pc_bridge_boot()` 在内核里现场加载同一个恶意 TU，逐条撞闸门并打印到 COM1：

```text
[PLUGIN-CORE] bridge boot: core init ok, router slots=512, plugins max=128
===== plugin_core kernel breach selftest (unsigned, real TU) =====
[PLUGIN-CORE] PASS  load_plugin(unsigned malicious plugin)
[PLUGIN-CORE] PASS  sandbox_root forced to /sandbox/evil.k64 (manifest said /etc)
[PLUGIN-CORE] PASS  A1 is_valid_user_ptr(0xDEADBEEF) == PC_ERR_DENIED
[REDLINE] forged wild pointer 0xdeadbeef  is_valid_user_ptr  addr=0xdeadbeef  rc=-5  errno=4  -> blocked, kernel alive
[REDLINE] open /etc/shadow  sandbox prefix check  rc=-8  errno=4  -> blocked, kernel alive
```

内核镜像**永不**执行 `evil_raw_deref`：裸写 `0xDEADBEEF` 在 ring0 是 #PF →
`fault_common` → 若没人兜住就是 triple fault。宿主测试用 `fork()` 隔离出
一个“插件进程”，内核侧则由 `nexos_compat.cpp` 的 IST + `longjmp` 兜底 ——
两者语义一致：**故障被限制在插件上下文内，系统继续运行**。

---

## 9. 并发压力与热拔插（阶段四）

`breach_test.cpp` 阶段四：16 线程 × 10000 次 = **160000 次** `dispatch_mcp`，
混合合法读、越权绝对路径、路径穿越、伪造用户指针；跑到第 48000 次（30%）
时热拔插。

确定性来自一条握手协议，而不是靠时序碰运气：

1. 恰好第 48000 次完成的那个线程置 `g_mid_reached=1` 并**原地自旋等待**。
2. 主线程卸载插件（`unload_plugin` 与 `dispatch_mcp` 互斥；正因为第 48000 次
   的调用已经返回，inflight 已归零，所以这里一次就拿到 `PC_OK`）。
   —— 这同时验证了半个 `PC_ERR_BUSY` 契约：另有 4a 用例专门制造 in-flight
   并断言卸载返回 `PC_ERR_BUSY`。
3. 主线程置 `g_unplug_done=1` 放行。此后 160000 − 48000 = **112000** 次调用
   必然看到 `PC_ERR_STALE`。

实测：

```text
[BREACH] PASS  evil.block is in-flight (inflight > 0)
[BREACH] PASS  unload while in-flight -> PC_ERR_BUSY
[BREACH] PASS  blocked tool completed after gate opened
[BREACH] PASS  unload after inflight drains -> OK
[BREACH] PASS  reload for stress phase
[BREACH] PASS  unload_plugin succeeded while 16 threads still dispatching
[BREACH] PASS  no deadlock: 16/16 threads joined
[BREACH] PASS  no unexpected return code under contention
[BREACH] PASS  both legal and malicious requests observed
[BREACH] PASS  old handle dispatches observed PC_ERR_STALE after unplug
[BREACH] PASS  exactly 160000 dispatches completed (none lost)
[BREACH] PASS  hot-plug happened while the load was still running
[BREACH] completed 160000 dispatches in 12 ms (ok=54000 sandbox=54000 stale=52000 other=0)
```

4a 用例本身也有两个陷阱：`evil.block` 的自旋窗口要靠**墙钟**（2 s）等，
不能用迭代计数（超卖的 VM 上“线程还没被调度”会被误读成“锁没生效”）；
而且必须等到 `plugin_snapshot().inflight > 0` 再尝试卸载，否则卸载可能
恰好落在调用之间的空隙里 —— 那时返回 `PC_OK` 是正确的，断言却会失败。

**关键不变量**（都写成了硬断言，不是“看起来没崩”）：

| 不变量 | 断言 |
|--------|------|
| 无死锁 | 16/16 `pthread_join` 成功；`alarm(300)` 是兜底看门狗 |
| 不丢请求 | 结果总数**恰好** 160000（多线程计数精确切分） |
| 旧句柄失效 | 卸载后 52000 次调用全为 `PC_ERR_STALE`，0 例外 |
| 防穿透 | 越权请求在并发路径上 0 次返回 `PC_OK`（否则计入 `other`） |
| 无异常返回码 | `other == 0`；合法请求 `ok=54000`，越权被拦 `sandbox=54000` |
| 槽位复用安全 | 复用后 `g_h != oldh`，旧句柄仍 `PC_ERR_STALE`，新句柄可用 |
| 幂等卸载 | 二次 `unload_plugin` 返回 `PC_ERR_STALE`（不是二次释放） |

---

## 10. 复现步骤

```bash
# 阶段一：容量门禁（必须先跑）
bash tools/verify/check_k64_fit.sh
make build/kernel64.bin K64_PLUGIN_CORE=1      # 期望 k64 undefined: 0 (clean)
bash tools/verify/check_kernel_sizes.sh

# 阶段三 + 四：宿主破坏性测试（40/40）
make plugin-core-breach

# 旧的机制层自测（68/68）
make plugin-core-selftest
```

阶段五（机制层自测）与阶段六（真实恶意插件破坏性测试）是两个独立目标：
前者验证机制本身的正确性，后者验证**在敌意输入下**这些机制真的守住了。
