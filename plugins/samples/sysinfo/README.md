# sysinfo —— 一个完整的 `.nexbundle` 示例插件

这是第四阶段的参考实现：一个**只读**的系统信息插件，演示从 C# 源码到
`.nexbundle` 的完整链路。它不申请网络、不申请文件写入，只申请
`sys.info` 一个能力位。

```
plugins/samples/sysinfo/
├── build.sh                        一键：扫描 -> 编译 -> .mex -> 打包 -> 校验
├── README.md                       本文件
├── src/
│   ├── SysInfoPlugin.csproj        MiniCLR 工程（/nostdlib, netstandard2.0）
│   ├── SysInfoTools.cs             [McpTool] 工具实现（sys_info / mem_info）
│   ├── PluginMain.cs               托管入口 Init / Dispatch
│   ├── SysInfoTools.Dispatch.g.cs  由 mcp_scan.py 生成，请勿手改
│   └── mcp_tools.json              由 mcp_scan.py 生成
├── bundle/                         打包输入目录（= .nexbundle 的解压形态）
│   ├── manifest.json               Bundle 清单（唯一必需入口）
│   ├── ui/layout.json              Bundle 级 UI（标签页 + 关于页）
│   └── plugins/sysinfo/
│       ├── plugin.json             单插件清单
│       ├── sysinfo.mex             托管插件（构建产物）
│       └── ui/
│           ├── layout.json         插件自己的窗口布局
│           └── preview.html        设计稿预览（不参与运行时）
└── dist/
    └── sysinfo.suite-1.0.0.nexbundle   最终分发包
```

## 1. 它如何融入 NexOS

```
┌──────────────────────── .nexbundle ────────────────────────┐
│ manifest.json          Bundle 元数据 + plugins[]           │
│ plugins/sysinfo/plugin.json   能力 / 沙盒 / 工具表          │
│ plugins/sysinfo/sysinfo.mex   MiniCLR 扁平程序集            │
│ plugins/sysinfo/ui/layout.json 声明式 UI                    │
└────────────────────────────┬───────────────────────────────┘
                             │ 安装（校验树哈希 / 可选 Ed25519）
                             ▼
     内核 plugin_loader.cpp                    宿主插件运行时
     ├─ 能力位授予（未签名摘 CAP_FS_WRITE）     ├─ Rust: 超时 / abort / panic 隔离
     ├─ 句柄 + 代际（旧句柄 -> PL_ERR_STALE）   ├─ MCP tools/list / tools/call
     └─ bnd_prefix_hit(/sandbox/<id>)           └─ allow_hosts 域名白名单
                             │
                             ▼
                 MCP 总线（JSON-RPC 2.0，零 IPC）
```

- **没有进程边界**：插件与宿主在同一进程、同一地址空间，通过函数指针 /
  托管方法调用。
- **隔离靠策略**：能力位 + 沙盒根 + 句柄代际。详见
  [`docs/MCP_PLUGIN_PROTOCOL.md`](../../../docs/MCP_PLUGIN_PROTOCOL.md)。
- **`sysinfo.mex` 的入口**：`plugin.json` 的
  `entry.init_symbol` / `entry.dispatch_symbol` 映射到
  `SysInfo.PluginMain::Init` / `::Dispatch`。宿主把 `Dispatch` 桥接成
  内核加载器的 `pl_call_fn`。

## 2. 构建

```sh
# 在仓库根目录
bash plugins/samples/sysinfo/build.sh

# 可选：同时签名（32 字节 Ed25519 种子，hex）
NEXBUNDLE_SIGN_KEY=9d61b1...7f60 bash plugins/samples/sysinfo/build.sh
```

要求：`python3`、.NET SDK 8+。不需要 g++ / cargo。

产物：

```
plugins/samples/sysinfo/dist/sysinfo.suite-1.0.0.nexbundle
```

单独校验 / 检视 / 解包：

```sh
python tools/image/nexbundle.py verify  <pkg> [--pubkey HEX] [--strict]
python tools/image/nexbundle.py inspect <pkg>
python tools/image/nexbundle.py unpack  <pkg> --out /tmp/sysinfo
python tools/image/nexbundle.py tree-hash <pkg>
```

## 3. 两个 MCP 工具

| 工具 | 能力 | 说明 |
| --- | --- | --- |
| `sys_info` | `sys.info` | CPU 占用、物理内存、页数、uptime、OS/CPU 标识、位数 |
| `mem_info` | `sys.info` | 物理页计数 + MiniCLR bump heap 高水位 |

`structuredContent` 字段（`ui/layout.json` 用 `/xxx` 绑定）：

| 字段 | 单位 | 来源 |
| --- | --- | --- |
| `cpu_load` | % (0..100) | `Host.CpuLoad()` |
| `mem_total_kb` / `mem_used_kb` / `mem_free_kb` | KiB | `Host.MemTotalKb()` / `Host.PagesUsed()` |
| `mem_used_pct` | % | 页数比值，整数 |
| `pages_total` / `pages_used` | 页（4 KiB） | `Host.Pages*()` |
| `uptime_ms` | ms | `Host.TickMs()` |
| `is_64bit` | 0/1 | `Host.Is64Bit()` |
| `os` / `cpu_vendor` | 字符串 | `Host.OsName()` / `Host.CpuVendor()` |
| `heap_alloc_bytes` / `heap_free_bytes` | 字节 | `Host.HeapAlloc()` / `HeapFree()` |

### 调用示例

`initialize`：

```json
{"jsonrpc":"2.0","id":0,"method":"initialize","params":{
  "protocolVersion":"2025-06-18","capabilities":{},
  "clientInfo":{"name":"nexos-shell","version":"1.0.0"}}}
```

`tools/list`（节选）：

```json
{"jsonrpc":"2.0","id":1,"method":"tools/list"}
```

```json
{"jsonrpc":"2.0","id":1,"result":{"tools":[
  {"name":"sys_info","description":"Read-only snapshot of CPU load, memory and OS identity.",
   "inputSchema":{"type":"object","properties":{},"additionalProperties":false},
   "x-nexos-plugin":"sysinfo"},
  {"name":"mem_info","description":"Physical page counters and the CLR bump-heap high-water marks.",
   "inputSchema":{"type":"object","properties":{},"additionalProperties":false},
   "x-nexos-plugin":"sysinfo"}]}}
```

`tools/call`：

```json
{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{
  "name":"sys_info","pluginId":"sysinfo","arguments":{}}}
```

```json
{"jsonrpc":"2.0","id":2,"result":{
  "content":[{"type":"text","text":"{\"cpu_load\":12,...}"}],
  "structuredContent":{"cpu_load":12,"mem_total_kb":1048576,
    "mem_used_kb":387072,"mem_free_kb":661504,"mem_used_pct":36,
    "pages_total":262144,"pages_used":96768,"uptime_ms":184329,
    "is_64bit":1,"os":"NexOS 2.1","cpu_vendor":"GenuineIntel"},
  "isError":false}}
```

## 4. UI 声明

`ui/layout.json` 只描述“有哪些控件、绑定哪个字段”，不写绘制代码：

```json
{"type":"metric","label":"CPU 占用","bind":"/cpu_load","unit":"%"}
```

`bind` 是 JSON Pointer，指向 `tools/call` 的 `structuredContent`。刷新由
`data.refresh_ms` 控制（本示例 1000 ms），按钮的 `action` 可以触发一次
显式调用。

`ui/preview.html` 是给评审看的静态预览，**不会**进运行时渲染路径。

## 5. 打包进 `.nexbundle`

`build.sh` 的第 4 步等价于：

```sh
python tools/image/nexbundle.py pack \
    --src plugins/samples/sysinfo/bundle \
    --out plugins/samples/sysinfo/dist/sysinfo.suite-1.0.0.nexbundle \
    --expect-id sysinfo.suite
```

`pack` 会强制检查：

- `manifest.json` 的 `id/name/version/plugins`；插件 id 与目录名一致；
- 每个插件的 `plugin.json` 存在，且 `id` / `version` 与 Bundle 清单一致；
- `entry.assembly` 指向的 `.mex` 在包内，且以 `MEX1` 魔数开头；
- `ui_layout` 指向的文件在包内；
- 能力名合法，`ipc` 直接拒绝；
- 所有成员 STORE、无路径穿越、无重复成员、体积在上限内。

`pack` 是**确定性**的：成员按名字排序、时间戳固定为 1980-01-01，同一输入
产出同一 SHA-256。

## 6. 安装后的权限

本包未签名时：

| 项目 | manifest 申请 | 实际授予 |
| --- | --- | --- |
| `caps` | `sys.info` | `sys.info` |
| `CAP_FS_WRITE` | 未申请 | 强制为 0 |
| 沙盒根 | 未声明 | `/sandbox/sysinfo` |
| 网络 | `[]` | 禁止所有出站连接 |

即使未来给本插件加上 `fs.write`，未签名安装也会被内核摘掉；签名安装时
`fs.write` 也只允许落在 `/sandbox/sysinfo` 之内。

## 7. 增加第二个插件

1. 复制 `plugins/sysinfo` 为 `plugins/<newid>`，改 `plugin.json` 的
   `id` / `version` / `entry`；
2. 在 `manifest.json` 的 `plugins[]` 里追加一项；
3. `build.sh` 里为它增加一次 `mex_pack.py`（或者改成遍历
   `bundle/plugins/*/`）；
4. `nexbundle.py pack/verify` —— 校验器会检查目录名、id、程序集三者一致。

这就是“一切皆插件”的含义：应用只是插件的组合，内核不认识“应用”这个
概念。
