# AI 工作台（AiWorkbench.mex）移植说明

> 把 `E:\cache\NexOSAI`（Python + pywebview + Ollama 的「NexOS AI Desktop」）
> 移植为 NexOS **独立应用**（`.mex`，kind 20，自己的 MiniCLR slot）。
> 版本：2026-11-13

---

## 1. 结论

| 项目 | 值 |
|---|---|
| 显示名 | AI 工作台 |
| `.mex` | `sfs_files/AiWorkbench.mex`（约 58 KB） |
| kind | **20**（`Kind.AiWorkbench` / `APP_AIWORKBENCH` = 25） |
| 图标 | 颜色 `0x5B3FD6`，字母 `W` |
| 源码 | `csharp/apps/standalone/parts/AiWorkbenchApp.cs` |
| 构建 | `tools/gen_standalone_apps.py` → `csharp/apps/standalone/AiWorkbench/` → `tools/standalone_apps.mk` |
| 模型 | 内核内置 GGUF 引擎（`Host.AiStart/AiPoll/AiStatus/AiCancel`） |

它不是 Python 运行时里的应用，而是和其它 19 个应用一样，由
`mforms_open_standalone()` 装载进独立 CLR slot，入口是 `App.Main::*`。

---

## 2. 功能映射

| 原 Python 应用 | NexOS 工作台 | 说明 |
|---|---|---|
| 多会话聊天（JSON 历史） | 4 个会话槽，`aiws1.txt` … `aiws4.txt` | 首行标题 + 正文；切换/结束自动保存到 MKFS |
| 会话标题 | 首条提问前 9 个字 | 新建时恢复「会话 N」 |
| 工作区文件上传/下载 | `ws_*` 文件（MKFS 根目录） | `/write`、`/read`、`/ls` |
| Agent 工具循环 | `[SHELL]/[WRITE]/[READ]/[DOC]/[LS]/[SEARCH]/[WEB]` 文本指令 | 回答完成后自动执行 1 个指令，结果回喂，最多 3 轮 |
| 本地搜索 | `/search` + `[SEARCH]` | 扫描 `ws_*` 与当前会话文本 |
| 联网搜索（Serper/博查） | `/web` + `[WEB]` | **尽力而为**：NexOS 无 TLS，只尝试明文 HTTP |
| 生成 PPTX/PDF | `/doc` + `[DOC]` → Markdown | 原生没有 PPTX/PDF 写入器；改为 `.md` 工作区文档 |
| 模型下拉框 / Ollama 地址 | 无 | 内核只加载一个内置 GGUF 模型 |
| 斜杠命令 | `/help /ls /read /write /doc /search /web /sh /new /clear /stop` | 不经过模型，模型未加载也可用 |

### 2.1 工具指令格式

模型在回答**最后单独一行**输出一条指令，工作台执行后把结果作为下一轮输入：

```
[SHELL] ls            # 只读白名单
[LS]                  # 列出 ws_* 文件
[READ] ws_note.txt
[WRITE] ws_note.txt|文件内容
[DOC] 标题|正文
[SEARCH] 关键词
[WEB] 关键词
```

解析是大小写不敏感的；同一段回答里出现多条时取**最后一条**。
`自动工具` 开关关闭后只保留 `/` 命令。

### 2.2 shell 白名单

`/sh` 与 `[SHELL]` 只放行只读动词：
`ls dir pwd echo help about meminfo ps netinfo netstat ver date time whoami users history`。
`cat` / `type` 被重定向到工作区读取，只能看到 `ws_*`。

> 原 Python 版给模型一个**无限制**的 shell（`subprocess` + `cwd`），
> 这里刻意收紧：模型是本地小模型，提示词注入的代价不该是整台机器。

---

## 3. 数据格式

### 3.1 会话文件（MKFS 根目录）

```
会话 1
你: 帮我总结 ws_note.txt
AI: ...
工具: READ ws_note.txt
...
```

* 文件大小上限受 `Host.ReadText` 的 `TEXT_MAX = 3072` 字节限制，
  内存里的 `history` 也截到 3200 字节，按键/按 UTF-8 边界裁剪。
* 只在「一轮回答结束 / 切换会话 / 新建 / 清空」时落盘一次，
  避免 MKFS `create()` 每次重写都消耗新的扇区（见 §5）。

### 3.2 工作区文件

* 文件名必须以 `ws_` 开头，长度 ≤ 19 字节（`FS_NAME_LEN = 20`）。
* `/doc` 生成 `ws_doc1.md` … `ws_doc9.md`（首个空闲槽）。
* 拒绝 `/`、`\`、`|`、`:`，不存在原版 `startswith(cwd)` 的路径穿越问题。

---

## 4. 与 Shell 的接线

| 位置 | 改动 |
|---|---|
| `sfs_files/apps.man` | `20|AI 工作台|AiWorkbench.mex|5B3FD6|W|...` |
| `csharp/apps/Shell/Shell.cs` | `Kind.AiWorkbench = 20` |
| `csharp/apps/Shell/Desktop.cs` | `KINDS = 21`、fallback 名称/颜色/字母、`LoadDefaults`、`KindStyle`、`KindForTarget` |
| `csharp/apps/standalone/Shared/Shim.cs` | kind 常量 + 名称兜底 |
| `app_registry.h` | `{ 25, 20, "AiWorkbench.mex" }` |
| `src/gui/gui_legacy.cpp` | `APP_AIWORKBENCH`、`managed_kind_for`/`app_for_managed_kind`、窗口 `760×540` |
| `tools/gen_standalone_apps.py` | `APPS` + `KIND_OF` |

`mforms_open(20)` → `apms_mex_for_kind(20)` → 装载 `AiWorkbench.mex`，
与其它应用走同一条链路，`mforms.cpp` 无需特判。

---

## 5. 已知限制

| 限制 | 原因 | 影响 |
|---|---|---|
| 无 PPTX / PDF | NexOS 没有对应写入器 | `/doc` 输出 Markdown，可在 Notepad 中查看 |
| 联网检索常不可用 | `Host.HttpGet` 无 TLS，只尝试明文 HTTP | 失败时明确回「联网检索不可用」，不编造结果 |
| 回答很短 | `ai_async_start(..., 96, ...)` 固定 96 token | 小模型 + 短预算，工具指令可能来不及输出；`/` 命令是可靠路径 |
| MKFS 不回收旧扇区 | `mkfs.create()` 先删旧文件再追加，`free_lba` 只增不减 | 会话反复保存会缓慢消耗数据盘；本轮按「每轮一次」控制写入 |
| 全局 cwd | 64 位 MKFS 只有一个 `g_cwd` | 所有工作区访问前调用 `Host.EnterPath(0, "")` 回到根目录 |
| 单模型 | 内核只加载一个 GGUF | 没有模型选择器 |

### 5.1 原 Python 版的问题（本移植规避）

* 聊天记录写在 exe 同目录 → Program Files 下无写权限；这里写 MKFS。
* HTTP 服务监听 `0.0.0.0` 且无鉴权 → 这里没有本地 HTTP 服务。
* `path.startswith(cwd)` 路径校验可被 `..` 绕过 → 这里用 `ws_` 前缀白名单。
* 前端 `innerHTML` 拼接模型输出 → 这里用 `Gfx.Text` 直接绘制纯文本。
* 工具调用字段名 `name` / `tool_name` 不一致 → 这里是纯文本指令，无 JSON schema。
* README 宣称 Serper / 博查，实际未实现 → 这里 `/web` 明确标注「尽力而为」。

---

## 6. 构建与验证

```sh
python tools/gen_standalone_apps.py
dotnet build csharp/apps/standalone/AiWorkbench/App.csproj -c Release
python tools/mex_pack.py \
  csharp/apps/standalone/AiWorkbench/bin/Release/AiWorkbench.dll \
  sfs_files/AiWorkbench.mex "App.Main::Init"
```

完整镜像（只能走 VM）：

```sh
vm-run "make build/os_v2.img MODEL_GGUF=build/qwen2-0_5b-instruct-q4_k_m.gguf"
tools/vbox_sync.ps1
python tools/vbox_check.py
```

运行时期望：桌面/开始菜单出现「AI 工作台」（紫色 W 图标），
窗口 760×540，四个会话槽、`/help`、`ws_*` 文件与 Markdown 文档可用。
