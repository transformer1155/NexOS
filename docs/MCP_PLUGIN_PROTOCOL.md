# NexOS 插件协议与 MCP 契约

> 版本：1（`schema_version: 1`）
> 状态：正式
> 适用实现：
> [`plugins/loader/plugin_loader.h`](../plugins/loader/plugin_loader.h)、
> [`src/plugin_runtime.rs`](../src/plugin_runtime.rs)、
> [`tools/image/nexbundle.py`](../tools/image/nexbundle.py)
> 相关文档：[`NEXBUNDLE.md`](NEXBUNDLE.md)、[`NEXPACK.md`](NEXPACK.md)、
> [`schemas/manifest.schema.json`](schemas/manifest.schema.json)

---

## 1. 五条不可协商的约束

这五条决定了下面所有设计，任何实现细节都不得违背：

| # | 约束 | 落地方式 |
| --- | --- | --- |
| 1 | **一切皆插件** | 只分发多插件 Bundle（`.nexbundle`）；不存在“单体应用”包 |
| 2 | **单进程、零 IPC** | 插件与宿主同进程；能力位 + 沙盒根 + 句柄代际做内存隔离，`CAP_IPC` 永远拒绝 |
| 3 | **MIT 开源** | 仓库根 `LICENSE`；所有示例与工具链同许可 |
| 4 | **原生 MCP 通信** | 对外是标准 JSON-RPC 2.0 / MCP；对内是函数指针或托管方法调用 |
| 5 | **内核态无标准库** | 内核加载器只依赖 `<stdint.h>`；内存/字符串全部手写（`memset_c` / `strcpy_c` / `strcmp_c` 等） |

“零 IPC”不是“没有通信”，而是**没有跨进程通信**。插件调用是同一次函数
调用栈里的直接调用，没有消息队列、共享内存段或 socketpair。

---

## 2. 术语

| 术语 | 含义 |
| --- | --- |
| Bundle | `.nexbundle` 包，包含一个或多个插件 |
| Plugin | 能力的载体，拥有独立 `id`、能力位、沙盒根、工具表 |
| Tool | 插件暴露的 MCP 工具，名字匹配 `^[a-z][a-z0-9_]{1,63}$` |
| Capability | 内核授予的权限位（`CAP_*`），manifest 里只是“申请” |
| Handle | 内核返回的 32 位插件句柄，编码了槽位与代际 |
| Sandbox | 插件可访问的路径前缀，默认 `/sandbox/<plugin_id>` |
| Host | 拥有内核加载器与 Rust 运行时的一方（NexOS 本体） |

---

## 3. Bundle 清单 `manifest.json`

`.nexbundle` 根目录的 `manifest.json` 是唯一入口。完整 JSON Schema 见
[`schemas/manifest.schema.json`](schemas/manifest.schema.json)。

### 3.1 顶层字段

| 字段 | 类型 | 必需 | 规则 |
| --- | --- | --- | --- |
| `schema_version` | integer | ✅ | 必须为 `1` |
| `id` | string | ✅ | `^[a-z][a-z0-9._-]{1,63}$`，全局唯一 |
| `name` | string | ✅ | 显示名，UTF-8 |
| `version` | string | ✅ | `MAJOR.MINOR.PATCH`，纯数字 |
| `publisher` | string | — | 发布者 |
| `description` | string | — | 一段话说明 |
| `license` | string | — | SPDX 标识，推荐 `MIT` |
| `homepage` | string | — | 项目地址 |
| `icon` | string | — | 包内 PNG 路径，≤64 KiB |
| `network` | object | ✅ | 必须显式写出，哪怕 `{"allow_hosts":[]}` |
| `capabilities` | string[] | ✅ | 必须显式写出，哪怕 `[]`；Bundle 级汇总 |
| `ui_layout` | string \| object | ✅ | 包内路径或内联布局 |
| `plugins` | array | ✅ | 非空；每个元素见 §3.2 |
| `signatures` | array | — | 保留字段；实际签名在 `signatures/ed25519.json` |

### 3.2 `plugins[]` 元素

| 字段 | 类型 | 必需 | 规则 |
| --- | --- | --- | --- |
| `id` | string | ✅ | `^[a-z][a-z0-9._-]{1,31}$`，必须等于目录名 |
| `name` | string | ✅ | 显示名 |
| `version` | string | ✅ | 必须等于 `plugins/<id>/plugin.json` 的 version |
| `description` | string | — | 说明 |
| `entry` | object | ✅ | 见 §3.3 |
| `network` | object | — | 缺省视为禁止所有网络 |
| `capabilities` | string[] | — | 缺省视为 `[]` |
| `ui_layout` | string \| object | — | 插件自己的窗口布局 |
| `tools` | array | — | 见 §3.4；托管插件通常至少一个 |

### 3.3 `entry`

| 字段 | 类型 | 适用 | 说明 |
| --- | --- | --- | --- |
| `kind` | string | 全部 | `managed` / `native` / `builtin` |
| `assembly` | string | `managed` | 插件目录下的单段 `.mex` 文件名 |
| `init_symbol` | string | `managed` | `Type::Method`，加载后调用一次 |
| `dispatch_symbol` | string | `managed` | `Type::Method`，桥接为 `pl_call_fn` |
| `symbol` | string | `native` | 原生入口符号（静态链接时） |

托管插件没有函数指针，所以 Host 用 `dispatch_symbol` 做桥接：

```c
/* plugin_loader.cpp 看到的 call_point */
static int bridge(uint32_t h, const char* tool, const char* args,
                  char* out, int outcap)
{
    /* clr_invoke("SysInfo.PluginMain::Dispatch", tool, args, out, outcap) */
}
```

### 3.4 `tools[]`

```json
{
  "name": "sys_info",
  "description": "Read-only snapshot of CPU load, memory and OS identity.",
  "inputSchema": {
    "type": "object",
    "properties": {},
    "additionalProperties": false
  }
}
```

- `name` 匹配 `^[a-z][a-z0-9_]{1,63}$`，同一 Bundle 内唯一；
- `description` 必填，给 LLM 与工具面板看；
- `inputSchema` 必须是 `type: "object"` 的 JSON Schema（2020-12 子集）；
- 构建期由 [`tools/misc/mcp_scan.py`](../tools/misc/mcp_scan.py) 从
  `[McpTool]` 生成，**不依赖运行时反射**（MiniCLR 没有 ECMA-335 元数据）。

### 3.5 `plugins/<id>/plugin.json`

结构与 `plugins[]` 元素相同，但 `id` / `version` 必须与 Bundle 清单一致，
`entry.assembly` 相对于 `plugins/<id>/`。校验器会比对，不一致直接拒绝打包。

---

## 4. 能力位

### 4.1 定义

| 名称 | 位 | 允许的操作 | 内核检查点 |
| --- | --- | --- | --- |
| `fs.read` | `0x00000001` | `fs_read` / `fs_list` / `fs_stat` | 路径必须落在沙盒根内 |
| `fs.write` | `0x00000002` | `fs_write` / `fs_delete` / `fs_mkdir` | 路径必须落在沙盒根内；**未签名永远为 0** |
| `net` | `0x00000004` | `net_http` / `net_fetch` / `net_dns` | 目标域名必须在 `allow_hosts` |
| `proc` | `0x00000008` | 读取进程 / 线程列表 | 只读 |
| `ui` | `0x00000010` | `ui_patch` / `ui_toast` | 只能改自己的窗口 |
| `ai` | `0x00000020` | `ai_infer` / `ai_embed` | 走本机 AI 引擎 |
| `dev` | `0x00000040` | `dev_ioctl` | 设备白名单 |
| `sys.info` | `0x00000080` | `sys_info` / `sys_tasks` / `sys_mem` | 只读快照 |
| `ipc` | `0x00000100` | — | **永远拒绝**：单进程零 IPC |

历史兼容：工具名同时接受 MCP 下划线形式（`sys_info`）与点分形式
（`sys.info`），映射到同一个位。

### 4.2 申请与授予

```text
manifest.capabilities   只是“申请”
        │
        ▼
  pl_load_plugin(m, is_signed, &h)
        │
        ├─ 未知位 / CAP_IPC        -> PL_ERR_CAP，拒绝加载
        ├─ 未签名                  -> caps &= ~CAP_FS_WRITE
        └─ 其余                    -> caps 原样授予
        │
        ▼
  PluginContext.caps        实际生效的权限
```

规则：

1. **未签名**插件不能写文件系统，即使 manifest 里申请了 `fs.write`。
2. **未签名**插件的沙盒根强制为 `/sandbox/<plugin_id>`，manifest 里写
   `/`、`/etc`、`/sandbox/other` 都不算数。
3. **签名**插件可以申请 `sandbox_root`，但必须满足：
   - 以 `/sandbox/` 开头；
   - 规范化后仍在 `/sandbox` 之内；
   - 否则静默回落到 `/sandbox/<plugin_id>`。
4. `ipc` 是保留位，任何 manifest 申请它都会导致加载失败。

### 4.3 纵深防御

宿主侧 [`src/plugin_runtime.rs`](../src/plugin_runtime.rs) 的 `CapSet` 是
内核能力位的镜像，在**派发前再检查一次**。即使内核策略被绕过，宿主也不会
执行越权工具。两处表必须同步维护。

---

## 5. 沙盒与路径边界

### 5.1 规范化

`bnd_prefix_hit(root, path)` 只接受：

- 绝对路径（`/` 开头）；
- 连续 `/` 折叠；
- `.` 段丢弃；
- **任何 `..` 段直接拒绝**（不做“回退后再比较”，避免解析歧义）；
- 反斜杠和控制字符直接拒绝；
- 结果不带结尾 `/`（根 `/` 除外，但根永远不会授予插件）。

### 5.2 前缀比较必须带边界

```
root = /sandbox/sysinfo
path = /sandbox/sysinfo/cache.txt     -> 允许
path = /sandbox/sysinfo2/x            -> 拒绝（不是 root 的前缀）
path = /sandbox/sysinfo/../../etc/x   -> 拒绝（含 ..）
```

比较逻辑：`path` 以 `root` 开头，且 `path[len(root)]` 是 `\0` 或 `/`。
这杜绝了 `sysinfo2` 这类“兄弟目录前缀”混淆。

### 5.3 读与写都检查

未签名插件通常保留 `CAP_FS_READ`，因此**读路径也必须过沙盒检查**。
只靠“摘掉写权限”是不够的：能读 `/etc/passwd` 一样是越权。

---

## 6. 句柄模型

### 6.1 编码

```text
handle = (generation << 8) | (slot_index + 1)

bit  0..7   : 槽位号 + 1（0 保留为“无效句柄”）
bit  8..31  : 代际（每次分配 +1，24 位回绕时跳过 0）
```

- `PL_MAX_PLUGINS = 128`，槽位用完后 `pl_load_plugin()` 返回
  `PL_ERR_NOSPACE`，绝不覆盖已加载插件。
- 查找 = 一次数组下标 + 一次代际比较，O(1)。
- 卸载或复用槽位都会 `Gen++`，旧句柄立即返回 `PL_ERR_STALE`，ABA 问题
  被代际封死。

### 6.2 生命周期

```
pl_init()
   │
   ├─ pl_load_plugin(manifest, signed, &h)
   │      ├─ 校验 id / 能力 / ABI
   │      ├─ 分配 PluginContext（kmalloc）
   │      ├─ 分配槽位，gen = old_gen + 1
   │      └─ 调用 entry_point(h, ui_layout, ...)，失败则自动回滚
   │
   ├─ mcp_dispatch(h, tool, args, out, cap)
   │      └─ busy++ / busy--，busy > 0 时禁止卸载
   │
   └─ pl_unload(h)
          ├─ busy > 0 -> PL_ERR_STATE
          ├─ 清空槽位、free(ctx)
          └─ 旧句柄从此 PL_ERR_STALE
```

### 6.3 错误码

| 常量 | 值 | 含义 |
| --- | --- | --- |
| `PL_OK` | 0 | 成功 |
| `PL_ERR_NOSPACE` | -1 | 128 个槽位已满 |
| `PL_ERR_BADARG` | -2 | 空指针 / 非法 id / 缺参数 |
| `PL_ERR_NOENT` | -3 | 插件或工具不存在 |
| `PL_ERR_STALE` | -4 | 句柄已悬空 |
| `PL_ERR_DENIED` | -5 | 能力位不足 |
| `PL_ERR_OVERFLOW` | -6 | 输出缓冲区不足 |
| `PL_ERR_CAP` | -7 | 非法能力组合（含 `ipc`） |
| `PL_ERR_SANDBOX` | -8 | 路径越界 |
| `PL_ERR_STATE` | -9 | 插件忙 / ABI 不匹配 |
| `PL_ERR_NET` | -10 | 域名不在白名单 |

---

## 7. MCP 传输：无状态 HTTP

### 7.1 为什么是无状态

NexOS 的插件运行时可能被嵌入桌面 Shell、终端或 AI 工作台。为了不强制
任何一方实现长连接状态机，Host 暴露**无状态 HTTP 端点**：

```
POST /mcp
Content-Type: application/json
Accept: application/json
X-NexOS-Client: shell/1.0.0
```

- 一个 HTTP 请求 = 一条 JSON-RPC 报文；
- 一个 HTTP 响应 = 一条 JSON-RPC 响应（通知返回 `202 Accepted`，无 body）；
- 不要求 `Mcp-Session-Id`；若客户端发送，Host 可以回显但不依赖它；
- **不实现 SSE**：`Accept: text/event-stream` 返回 `406 Not Acceptable`；
- 批量（batch）请求在 v1 中**不支持**，收到数组返回 `-32600`。

绑定地址必须是回环（`127.0.0.1` / `::1`），或由 Host 的 ACL 限制到
可信客户端。协议本身不提供跨机器认证。

### 7.2 HTTP 状态码映射

| 情况 | HTTP | body |
| --- | --- | --- |
| 正常请求 | 200 | JSON-RPC 响应 |
| 通知 | 202 | 空 |
| JSON 解析失败 | 200 | `-32700` 响应（便于客户端统一处理） |
| 非 POST | 405 | `{"error":"method not allowed"}` |
| 非 JSON Content-Type | 415 | `{"error":"unsupported media type"}` |
| 请求体 > 1 MiB | 413 | `{"error":"payload too large"}` |

---

## 8. 生命周期与报文

### 8.1 `initialize`

请求：

```json
{
  "jsonrpc": "2.0",
  "id": 0,
  "method": "initialize",
  "params": {
    "protocolVersion": "2025-06-18",
    "capabilities": {},
    "clientInfo": { "name": "nexos-shell", "version": "1.0.0" }
  }
}
```

响应：

```json
{
  "jsonrpc": "2.0",
  "id": 0,
  "result": {
    "protocolVersion": "2025-06-18",
    "capabilities": { "tools": { "listChanged": true } },
    "serverInfo": { "name": "nexos-plugin-runtime", "version": "0.1.0" },
    "instructions": "NexOS single-process plugin runtime. Tools are namespaced by x-nexos-plugin."
  }
}
```

客户端随后发送通知（无 `id`，不得回包）：

```json
{ "jsonrpc": "2.0", "method": "notifications/initialized" }
```

### 8.2 `ping`

```json
{"jsonrpc":"2.0","id":9,"method":"ping"}
```

```json
{"jsonrpc":"2.0","id":9,"result":{}}
```

### 8.3 `tools/list`

请求：

```json
{ "jsonrpc": "2.0", "id": 1, "method": "tools/list" }
```

响应（每个工具带扩展字段 `x-nexos-plugin`，用于消歧）：

```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": {
    "tools": [
      {
        "name": "sys_info",
        "description": "Read-only snapshot of CPU load, memory and OS identity.",
        "inputSchema": {
          "type": "object",
          "properties": {},
          "additionalProperties": false
        },
        "x-nexos-plugin": "sysinfo"
      },
      {
        "name": "mem_info",
        "description": "Physical page counters and the CLR bump-heap high-water marks.",
        "inputSchema": {
          "type": "object",
          "properties": {},
          "additionalProperties": false
        },
        "x-nexos-plugin": "sysinfo"
      }
    ]
  }
}
```

### 8.4 `tools/call`

请求：

```json
{
  "jsonrpc": "2.0",
  "id": 2,
  "method": "tools/call",
  "params": {
    "name": "sys_info",
    "pluginId": "sysinfo",
    "arguments": {}
  }
}
```

- `pluginId` 可选。省略时按工具名反查；
  - 命中 0 个 → `-32004`；
  - 命中多个 → `-32602`，要求客户端显式传 `pluginId`。
- `arguments` 必须是对象；缺省视为 `{}`。

成功响应：

```json
{
  "jsonrpc": "2.0",
  "id": 2,
  "result": {
    "content": [
      {
        "type": "text",
        "text": "{\"cpu_load\":12,\"mem_total_kb\":1048576,\"mem_used_kb\":387072,\"mem_free_kb\":661504,\"mem_used_pct\":36,\"pages_total\":262144,\"pages_used\":96768,\"uptime_ms\":184329,\"is_64bit\":1,\"os\":\"NexOS 2.1\",\"cpu_vendor\":\"GenuineIntel\"}"
      }
    ],
    "structuredContent": {
      "cpu_load": 12,
      "mem_total_kb": 1048576,
      "mem_used_kb": 387072,
      "mem_free_kb": 661504,
      "mem_used_pct": 36,
      "pages_total": 262144,
      "pages_used": 96768,
      "uptime_ms": 184329,
      "is_64bit": 1,
      "os": "NexOS 2.1",
      "cpu_vendor": "GenuineIntel"
    },
    "isError": false
  }
}
```

`content` 给 LLM / 人类看，`structuredContent` 给 UI 用（`ui_layout` 的
`bind` 指向后者）。二者可以同时存在。

### 8.5 业务错误 vs 协议错误

- **协议错误**用 JSON-RPC `error` 对象（找不到工具、参数不合法、超时等）；
- **业务错误**用 `result.isError: true` + 文本，HTTP 仍是 200。

```json
{
  "jsonrpc": "2.0",
  "id": 3,
  "result": {
    "content": [{ "type": "text", "text": "device not ready" }],
    "structuredContent": null,
    "isError": true
  }
}
```

插件内部失败（例如传感器读不到）应使用业务错误；只有“这次调用根本无法
成立”才用协议错误。

---

## 9. 错误码总表

| 码 | 名称 | 触发条件 |
| --- | --- | --- |
| `-32700` | Parse error | body 不是合法 JSON |
| `-32600` | Invalid Request | 缺 `jsonrpc` / `method`，或 batch 数组 |
| `-32601` | Method not found | 未知 `method` |
| `-32602` | Invalid params | 缺 `params.name`、`arguments` 不是对象、重复工具、未知能力 |
| `-32603` | Internal error | 未分类的宿主错误 |
| `-32000` | Busy | 并发调用超过上限（默认 32） |
| `-32001` | Policy denied | 能力位不足、路径越界、域名不在白名单 |
| `-32002` | Stale handle | 插件已卸载 / 句柄代际不匹配 |
| `-32003` | Timeout | 单次调用超过 `call_timeout`（默认 30 s） |
| `-32004` | Not found | 未知插件 / 未知工具 |
| `-32005` | Plugin panic | 插件 panic 被 `catch_unwind` 捕获 |
| `-32800` | Cancelled | 客户端取消或任务被 abort |

内核 `mcp_dispatch()` 的映射：

| 内核返回 | JSON-RPC |
| --- | --- |
| `PL_ERR_BADARG` / `PL_ERR_OVERFLOW` | `-32602` |
| `PL_ERR_DENIED` / `PL_ERR_SANDBOX` / `PL_ERR_NET` / `PL_ERR_CAP` | `-32001` |
| `PL_ERR_NOENT` | `-32004` |
| `PL_ERR_STALE` | `-32002` |
| 其它 | `-32603` |

### 9.1 错误示例

**未知工具：**

```json
{
  "jsonrpc": "2.0",
  "id": 4,
  "error": {
    "code": -32004,
    "message": "unknown tool \"format_disk\" (pass params.pluginId to disambiguate)"
  }
}
```

**能力不足：**

```json
{
  "jsonrpc": "2.0",
  "id": 5,
  "error": {
    "code": -32001,
    "message": "plugin \"sysinfo\" lacks capability 0x2 required by \"fs_write\"",
    "data": { "kind": "capability_denied" }
  }
}
```

**沙盒越界（内核直接返回）：**

```json
{
  "jsonrpc": "2.0",
  "id": 6,
  "error": {
    "code": -32001,
    "message": "path escapes sandbox root",
    "data": {
      "errno": -8,
      "plugin": "sysinfo",
      "tool": "fs_write",
      "path": "/sandbox/sysinfo/../../etc/shadow"
    }
  }
}
```

**域名不在白名单：**

```json
{
  "jsonrpc": "2.0",
  "id": 7,
  "error": {
    "code": -32001,
    "message": "host not in allow_hosts",
    "data": {
      "errno": -10,
      "plugin": "weather",
      "tool": "net_http",
      "path": "https://evil.example/x"
    }
  }
}
```

**超时：**

```json
{
  "jsonrpc": "2.0",
  "id": 8,
  "error": {
    "code": -32003,
    "message": "plugin call timed out after 30s",
    "data": { "kind": "timeout" }
  }
}
```

**panic：**

```json
{
  "jsonrpc": "2.0",
  "id": 9,
  "result": {
    "content": [
      { "type": "text", "text": "plugin sysinfo/sys_info panicked: boom from plugin" }
    ],
    "structuredContent": null,
    "isError": true
  }
}
```

> 注意：panic 是**插件 bug**，不是协议错误。运行时把它变成
> `isError: true` 的结果，宿主继续服务，而不是让整个进程崩溃。

**通知不回包：**

```http
POST /mcp HTTP/1.1
Content-Type: application/json

{"jsonrpc":"2.0","method":"notifications/initialized"}
```

```http
HTTP/1.1 202 Accepted
Content-Length: 0
```

---

## 10. 超时、取消与并发

### 10.1 超时

- 默认 `call_timeout = 30 s`，可由 Host 的 `RuntimeLimits` 配置；
- 超时后运行时先 `JoinHandle::abort()`，再 `await` 句柄，确保任务已被
  drop 才返回 `-32003`；
- `abort` 只在 `.await` 点生效。**插件 handler 必须是 async**，长 CPU
  计算要主动 `tokio::task::yield_now().await`，否则无法被抢占。这条是
  插件作者的强制义务。

### 10.2 取消

客户端可以发送 MCP 取消通知：

```json
{ "jsonrpc": "2.0", "method": "notifications/cancelled",
  "params": { "requestId": 2, "reason": "user navigated away" } }
```

Host 查到对应的在飞任务后 abort，调用方收到 `-32800`。取消通知本身没有
响应。

### 10.3 并发上限

- 默认 `max_concurrent_calls = 32`；
- 超过上限立刻返回 `-32000 Busy`，**不排队**，避免慢插件把内存吃光；
- 并发许可在任务 drop 时自动归还（`OwnedSemaphorePermit`）。

### 10.4 panic 隔离

```rust
match CatchUnwind::new(handler(ctx, args)).await {
    Ok(result) => result,
    Err(payload) => ToolResult::error(format!("plugin panicked: ...")),
}
```

`catch_unwind` 在 `panic=abort` 下是空操作，因此 crate 里强制：

```rust
#[cfg(panic = "abort")]
compile_error!("nexos-plugin-runtime must be built with panic=unwind");
```

---

## 11. `ui_layout` 规范

### 11.1 两种形态

```json
"ui_layout": "ui/layout.json"
```

或内联：

```json
"ui_layout": { "schema_version": 1, "window": { ... }, "root": { ... } }
```

路径相对于 Bundle 根；插件级路径也可以相对于 `plugins/<id>/`。校验器会
同时尝试两种前缀。

### 11.2 顶层

```json
{
  "schema_version": 1,
  "window": {
    "title": "系统信息",
    "width": 520,
    "height": 380,
    "resizable": true
  },
  "data": {
    "tool": "sys_info",
    "plugin": "sysinfo",
    "refresh_ms": 1000
  },
  "root": { "type": "column", "children": [] }
}
```

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `schema_version` | integer | 必须为 `1` |
| `window.title` | string | 窗口标题 |
| `window.width/height` | integer | 初始尺寸（像素） |
| `window.resizable` | boolean | 是否可缩放 |
| `data.tool` | string | 默认刷新的工具名 |
| `data.plugin` | string | 工具所属插件（可选） |
| `data.refresh_ms` | integer | 自动刷新周期；`0` 表示不自动刷新 |
| `root` | object | 控件树根节点 |

### 11.3 控件

v1 定义以下 `type`。未知类型**必须被忽略**（不是报错），以便向前兼容。

| `type` | 关键字段 | 说明 |
| --- | --- | --- |
| `column` | `children`, `gap`, `padding` | 纵向排列 |
| `row` | `children`, `gap` | 横向排列 |
| `panel` | `label`, `children`, `layout`, `plugin` | 分组 / 标签页内容 |
| `tabs` | `children`（每个是 `panel`） | 标签页容器 |
| `metric` | `label`, `bind`, `unit` | 大号数值卡片 |
| `progress` | `label`, `bind`, `max` | 进度条 |
| `keyvalue` | `items[]`（`label`,`bind`/`value`,`unit`） | 两列键值表 |
| `text` | `text` | 静态文本 |
| `heading` | `text` | 标题 |
| `badge` | `label`, `bind`, `true_text`, `false_text` | 布尔徽标 |
| `button` | `label`, `action` | 触发一次 `tools/call` |
| `spacer` | — | 弹性空白 |
| `list` | `bind`, `item_bind` | 数组列表（v1 可选实现） |

### 11.4 绑定

- `bind` 是 JSON Pointer，指向 `structuredContent`：

  ```json
  { "type": "metric", "label": "CPU 占用", "bind": "/cpu_load", "unit": "%" }
  ```

- 绑定失败（字段缺失）显示 `—`，不抛错；
- `action` 触发调用：

  ```json
  { "type": "button", "label": "刷新",
    "action": { "tool": "sys_info", "arguments": {} } }
  ```

- UI 只能读 `structuredContent`，不能直接读内核内存。

---

## 12. 版本演进

| 变化 | 兼容性要求 |
| --- | --- |
| 新增可选 manifest 字段 | 旧 Host 忽略，保持兼容 |
| 新增能力位 | 旧 Host 应拒绝未知位（fail closed） |
| 新增工具 | 通过 `tools/list` 动态发现，兼容 |
| 修改工具 `inputSchema` | 提升插件 `MINOR` |
| 破坏性协议变更 | 提升 `schema_version` 与 MCP `protocolVersion` |
| `ipc` | 永远不会解禁 |

ABI：`PluginManifest.abi_version` 当前为 `1`；`0` 视为 `1`；大于 `1`
返回 `PL_ERR_STATE`。内核只加载它认识的 ABI。

---

## 13. 安全清单（实现与审计都用）

- [ ] 未签名插件：`CAP_FS_WRITE` 已摘除；
- [ ] 未签名插件：沙盒根等于 `/sandbox/<id>`；
- [ ] 读路径与写路径都过 `bnd_prefix_hit`；
- [ ] `..` / 反斜杠 / 控制字符直接拒绝，不做“修正”；
- [ ] 前缀比较带 `/` 或 `\0` 边界；
- [ ] `allow_hosts` 为空 = 禁止所有网络（fail closed）；
- [ ] 通配符只允许 `*.domain`，且不匹配 apex；
- [ ] 每个插件的工具名在 Bundle 内唯一；
- [ ] 句柄代际在卸载和槽位复用时都递增；
- [ ] `busy > 0` 时禁止卸载；
- [ ] 超时后先 abort 再 await；
- [ ] `panic=unwind`，每次 poll 都被 `catch_unwind` 包住；
- [ ] `.nexbundle` 所有成员 STORE，无路径穿越；
- [ ] 签名校验覆盖 `tree_sha256`，且 subject 的 id/version 与 manifest 一致。
