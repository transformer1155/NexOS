# NexOS 应用包格式规范 —— `.nexpack`

> 版本：1（`schema_version: 1`）
> 状态：正式
> 相关实现：[`tools/nexpack.py`](../tools/nexpack.py)（打包/解包/校验）、
> [`csharp/winhost/PackageInstaller.cs`](../csharp/winhost/PackageInstaller.cs)（安装器）、
> [`csharp/NexOS.Core/McpSdk.cs`](../csharp/NexOS.Core/McpSdk.cs)（MCP SDK）

---

## 1. 设计目标

NexOS 的第三方应用需要一套**可分发、可校验、可安装**的包格式，对应 VS Code 的 `.vsix`。
`.nexpack` 就是 NexOS 的应用包扩展名。

设计受以下**内核现实约束**直接决定（不是随意选择）：

| 约束 | 来源 | 对格式的影响 |
| --- | --- | --- |
| 内核**没有 zlib/inflate** | `plugins/vnc_client.cpp:393` 明确注释 | 包内成员**必须 STORE（不压缩）**，否则内核侧无法读取 |
| 内核**没有二进制写原语** | 只有 `kern_fs_read`，无 `kern_fs_write` | 安装由**托管侧 / 宿主侧**完成，内核只读结果 |
| **SFS 只读** | `apps.json` 在 SFS，VM 内不可写 | 安装产物落在 **MKFS**，注册表走 MKFS overlay |
| MiniCLR **无反射** | `clr.cpp` / `mex_pack.py` 扁平化设计 | MCP 工具列表**构建期**由 `tools/mcp_scan.py` 生成 |
| 内存受限 | 32 位内核堆 | 单包建议 ≤ 4 MiB，图标 ≤ 64 KiB |

---

## 2. 容器格式

`.nexpack` 是一个标准 **ZIP 归档**，但所有成员**必须使用 STORE 方法（method 0，不压缩）**。

选择 ZIP 而非自定义容器的理由：

- Python `zipfile`、.NET `System.IO.Compression` 都原生支持，工具链零依赖；
- STORE 模式下 ZIP 只是"带目录的拼接"，内核若将来加解析器只需顺序扫本地文件头；
- 便于 `unzip -l` 等通用工具检视。

### 2.1 成员布局

```
example.nexpack
├── manifest.json          # 必需，包元数据 + MCP 工具描述
├── app.mex                # 必需，托管程序集（mex_pack.py 产物）
└── icon.png               # 可选，桌面图标（PNG，≤ 64 KiB）
```

文件名**大小写敏感**，且必须位于归档**根目录**（不允许子目录前缀）。

### 2.2 为什么 `.mex` 与 `manifest.json` 分离

`app.mex` 是 `mex_pack.py` 扁平化后的**字节码镜像**，不含可读元数据（MiniCLR 无反射）。
因此**所有**人类可读信息——应用名、版本、作者、MCP 工具签名——都必须放在 `manifest.json`。
这是 MiniCLR 无反射约束的直接结果。

---

## 3. `manifest.json` 规范

### 3.1 完整示例

```json
{
  "schema_version": 1,
  "app_id": "weather",
  "name": "Weather App",
  "version": "1.0.0",
  "author": "nexos",
  "description": "Show current weather for a location.",
  "icon": "icon.png",
  "entry": {
    "kind": 13,
    "desktop": true,
    "taskbar": false
  },
  "ai_mode_required": "Fast",
  "mcp_server": [
    {
      "tool_name": "get_weather",
      "description": "Return current weather for a location.",
      "parameters_schema": {
        "type": "object",
        "properties": {
          "loc": { "type": "string", "description": "City or coordinates" }
        },
        "required": ["loc"]
      }
    }
  ],
  "mcp_client_access": []
}
```

### 3.2 字段说明

| 字段 | 类型 | 必需 | 说明 |
| --- | --- | --- | --- |
| `schema_version` | int | 是 | 必须是 `1`。安装器拒绝其它值。 |
| `app_id` | string | 是 | 唯一标识。**必须**匹配 `^[a-z][a-z0-9_]{1,31}$`，且等于 `app.mex` 的 basename。 |
| `name` | string | 是 | 显示名，展示在桌面 / 开始菜单。 |
| `version` | string | 是 | `MAJOR.MINOR.PATCH`。 |
| `author` | string | 是 | 作者/组织。 |
| `description` | string | 否 | 一句话描述。 |
| `icon` | string | 否 | 归档内图标文件名，默认 `icon.png`。 |
| `entry` | object | 否 | 桌面入口配置，见 3.3。 |
| `ai_mode_required` | string | 否 | `"Fast"` / `"Balanced"` / `"Heavy"`，默认 `"Balanced"`。 |
| `mcp_server` | array | 否 | 本应用**提供**的 MCP 工具。 |
| `mcp_client_access` | array | 否 | 本应用**可调用**哪些 `app_id` 的工具（ACL）。 |

### 3.3 `entry` 对象

| 字段 | 类型 | 默认 | 说明 |
| --- | --- | --- | --- |
| `kind` | int | `13` | 启动时传给 `Host.OpenApp(kind)` 的 Kind 编号。 |
| `desktop` | bool | `true` | 是否在桌面创建 `.lnk` 快捷方式。 |
| `taskbar` | bool | `false` | 是否固定到任务栏。 |

> **为什么 `kind` 存在**：MiniCLR 的窗口系统以 `Kind`（int）为索引，`Host.OpenApp(kind)`
> 是唯一的应用启动通道（见 `Shell.cs:96`）。第三方应用复用已有 Kind，或使用
> `>= 100` 的扩展区（见第 6 节）。

### 3.4 `mcp_server[]` 元素

| 字段 | 类型 | 必需 | 说明 |
| --- | --- | --- | --- |
| `tool_name` | string | 是 | `^[a-z][a-z0-9_]{1,63}$`，在**本应用内**唯一。 |
| `description` | string | 是 | 提供给 LLM 的工具说明。 |
| `parameters_schema` | object | 是 | JSON Schema（`type: "object"`）。 |

对应内核结构 `McpTool`（`nexos_ai.h`），字段一一映射。

---

## 4. 打包与校验

### 4.1 打包

```bash
python tools/nexpack.py pack \
    --mex   sfs_files/Weather.mex \
    --manifest manifest.json \
    --icon  icon.png \
    --out   dist/weather-1.0.0.nexpack
```

工具会：
1. 校验 `manifest.json` 全部字段（含 `app_id` 与 `.mex` basename 一致性）；
2. 校验 `app.mex` 是合法 `.mex`（魔数 `MEX1`，见 `tools/mex_dump.py`）；
3. 以 **STORE** 方式写入 ZIP；
4. 计算并打印每个成员的 CRC32 与 SHA-256。

### 4.2 校验

```bash
python tools/nexpack.py verify dist/weather-1.0.0.nexpack
```

退出码 `0` 表示通过，非 `0` 表示失败（并打印具体原因）。

### 4.3 检视

```bash
python tools/nexpack.py inspect dist/weather-1.0.0.nexpack
```

打印 manifest 摘要、成员列表、大小、以及将被注册的 MCP 工具。

---

## 5. 安装流程（`PackageInstaller`）

安装器运行在**托管侧**（WinHost 宿主或 VM 内的托管 shell）。内核不参与解包。

```
.nexpack 文件
   │
   ├─ 1. 打开 ZIP（内存/文件）
   ├─ 2. 读取并校验 manifest.json（schema_version / app_id / 字段正则）
   ├─ 3. 校验 app.mex 存在且魔数正确
   ├─ 4. 写入 MKFS：
   │      mkfs/<app_id>.mex          ← app.mex 原始字节
   │      mkfs/<app_id>.icon         ← icon.png 原始字节（可选）
   ├─ 5. 合并注册表：
   │      mkfs/apps.json             ← 系统 apps.json + 本包 manifest 条目
   ├─ 6. 桌面入口（entry.desktop == true）：
   │      mkfs/Desktop/<name>.lnk    ← 内容为十进制 Kind 编号
   └─ 7. 刷新桌面（FileRefresh + SyncFromFs）
```

### 5.1 注册表合并语义

`mkfs/apps.json` 是**覆盖层（overlay）**，不是完整替换：

- 内核启动时先读系统 `/apps.json`（SFS），再读 MKFS 的 `apps.json`；
- 合并键是 `app_id`：**已存在则覆盖**，不存在则新增；
- 因此安装第三方应用**不会**破坏内置应用（`weather` / `assistant`）的注册。

### 5.2 幂等性与升级

- 重复安装同一 `app_id`：覆盖 `.mex` / `.icon` / 注册表条目（等价于升级）。
- 版本比较用 `MAJOR.MINOR.PATCH` 数值三元组；安装器打印 `old -> new`。
- 卸载：删除 `mkfs/<app_id>.mex`、`.icon`、对应 `.lnk`，并从 `mkfs/apps.json` 移除该 `app_id`。

### 5.3 失败回滚

安装器按"**先全部校验，再落盘**"顺序执行：

1. 阶段 A（只读）：解包 + 校验 manifest + 校验 `.mex` + 解析现有注册表；
2. 阶段 B（写入）：写文件 + 写注册表。

阶段 A 任一步失败 → 磁盘无任何改动，返回错误码。
阶段 B 中注册表写入失败 → 删除本次已写入的文件（尽力回滚）。

---

## 6. 桌面入口与 Kind 扩展

`Desktop.cs` 的 `Kind` 是编译期常量（`0..13`，`KINDS = 14`），第三方应用**不能**动态新增 Kind。
因此安装器采用两种策略之一：

| 策略 | 条件 | 行为 |
| --- | --- | --- |
| **复用 Kind** | `entry.kind < 14` | `.lnk` 体写该 Kind，双击走既有窗口路径。 |
| **扩展区** | `100 <= entry.kind` | 由宿主注册"扩展 Kind 处理器"，启动时按 `app_id` 调 `clrapp <app_id>.mex`。 |

`.lnk` 文件体格式保持兼容：**十进制数字符串**（`Desktop.cs:621 ParseKind`）。

---

## 7. MCP SDK 与构建期工具发现

MiniCLR 无反射，因此 `[McpTool]` 特性**不能在运行时枚举**。流程改为构建期扫描：

```
McpServer 子类源码
   │  [McpTool("get_weather", Description="...")]
   │  public static string GetWeather(string json_args)
   ▼
tools/mcp_scan.py            ← Roslyn 不可用，改用语法扫描（正则 + 括号配平）
   │  产出 mcp_tools.json
   ▼
manifest.json 的 mcp_server[]  ← 由 nexpack.py pack 合并进包
   ▼
mkfs/apps.json                            ← 安装时注册
   ▼
内核 AppRegistry.mcp_server[]             ← ai_gateway_init 加载
   ▼
mcp_dispatch_request → clr_call("App.Tools::Dispatch", ...)  ← 原生→托管派发
```

### 7.1 SDK 类型（`NexOS.Core/McpSdk.cs`）

```csharp
// 标记一个静态方法为 MCP 工具。构建期被 tools/mcp_scan.py 发现。
public sealed class McpToolAttribute : Attribute
{
    public string Name;        // 工具名；null 则用方法名转 snake_case
    public string Description; // 给 LLM 的说明
    public McpToolAttribute(string name = null) { Name = name; }
}

// 第三方应用继承此类，用 [McpTool] 标记要暴露的方法。
public abstract class McpServerBase
{
    public abstract string AppId { get; }
    public virtual void OnInit() { }
    // 构建期扫描生成的派发表调用的统一入口：
    //   tool_name -> 具体方法
    public abstract string Invoke(string tool_name, string json_args);
}
```

`Invoke` 由 `tools/mcp_scan.py --emit-dispatch` 生成一个 `partial` 实现，
这样既不需要反射，也不需要开发者手写 if-else 链。

### 7.2 派发链路

内核 `mcp_dispatch_request` 命中 `mcp_server[]` 后，调用该工具的 `dispatch` 函数指针。
对第三方应用，`generic_dispatch` 的实现是：

```cpp
const char* managed_dispatch(const char* src, const char* tgt,
                             const char* tool, const char* args)
{
    // 只有 tgt 是当前常驻程序集时才可能成功（MiniCLR 单常驻限制）
    if (strcmp(tgt, g_current_app_id) != 0)
        return "{\"code\":5,\"error\":\"target not resident\"}";
    return clr_call_string("NexOS.Mcp.ManagedHost::Dispatch",
                           tgt, tool, args);
}
```

> **已知限制（必须写进文档）**：MiniCLR 同时只常驻**一个**托管程序集
> （`clr_run_resident`）。因此第三方应用的 MCP 工具只能在该应用**正在运行时**被派发；
> 未运行时调用返回 `code:5`。这是单常驻模型的固有限制，不是实现缺陷。

---

## 8. 安全模型

| 威胁 | 缓解 |
| --- | --- |
| 冒充调用方绕过 ACL | 内核用 `g_current_app_id`（常驻 `.mex` basename），托管侧无法伪造。 |
| 恶意包覆盖内置应用 | 安装器拒绝安装 `app_id` 属于保留集合（`weather`/`assistant`/`shell`）的包，除非显式 `--force`。 |
| 超大包耗尽内存 | 安装器限制：包 ≤ 4 MiB，`.mex` ≤ 2 MiB，图标 ≤ 64 KiB。 |
| 路径穿越（`../`） | 归档内成员名必须是**单段文件名**，拒绝任何 `/`、`\`、`..`。 |
| manifest 注入 | `app_id` / `tool_name` 正则白名单；JSON 解析失败即拒绝。 |

---

## 9. 版本演进

- `schema_version` 单调递增；安装器接受 `<=` 自身支持的最大版本。
- 新字段一律**可选**，缺失时取默认值——保证旧安装器能装新包（忽略未知字段）。
- 破坏性变更必须提升 `schema_version`，并在本文件新增"版本 2 迁移说明"。

---

## 10. 参考实现索引

| 组件 | 路径 |
| --- | --- |
| 打包 / 解包 / 校验 CLI | `tools/nexpack.py` |
| 构建期 MCP 工具扫描 | `tools/mcp_scan.py` |
| MCP SDK（`McpServerBase` / `[McpTool]`） | `csharp/NexOS.Core/McpSdk.cs` |
| 安装器 | `csharp/winhost/PackageInstaller.cs` |
| 内核注册表加载 | `nexos_ai.cpp` → `ai_gateway_init` |
| 内核 MCP 派发 | `nexos_ai.cpp` → `mcp_dispatch_request` |
