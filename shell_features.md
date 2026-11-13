# NexOS 托管壳（Managed Shell）功能清单

> 生成日期：2026-11-12 ｜ 最近同步：2026-11-12 深夜构建（**20 个应用全部独立 .mex** · 清单驱动 `apps.man` · **APMS 内核运维**）｜ 范围：`csharp/apps/Shell/*` + `csharp/apps/standalone/*` + `csharp/NexOS.Forms/*` + 内核 `.attic64/apms.inc` + 原生 GUI 桥 `src/gui/gui_legacy.cpp`

> 每个功能下均留有一个 **Bug** 行，供自行填写。对我说明「查看功能清单」时，我会逐条读取并据此修复。


## 应用（Kind 枚举，共 20 个 · 全部为独立 .mex 程序集）


### 控制面板  `Kind 0`  【正常】

系统设置入口，含个性化、任务栏、显示、网络、账户等子页；可由 `Shell.OpenSettings(page)` 预导航到指定页。


- 标识：`Kind.ControlPanel` ｜ 原生 AppType：`APP_CONTROL_PANEL` ｜ 程序集：`ControlPanel.mex`（独立 .mex）

- **Bug（待填写）**：



### 文件资源管理器  `Kind 1`  【正常】

浏览 MKFS / 桌面卷文件，支持打开、右键菜单、跨窗口拖放文件。


- 标识：`Kind.FileExplorer` ｜ 原生 AppType：`APP_FILE_EXPLORER` ｜ 程序集：`FileExplorer.mex`（独立 .mex）

- **Bug（待填写）**：



### 任务管理器  `Kind 2`  【正常】

查看运行进程/窗口，结束任务；任务栏右键「结束进程」调用 `Shell.CloseApp`。


- 标识：`Kind.TaskManager` ｜ 原生 AppType：`APP_TASK_MANAGER` ｜ 程序集：`TaskManager.mex`（独立 .mex）

- **Bug（待填写）**：



### 终端  `Kind 3`  【正常】

C# GUI 终端（支持中文 IME）。桌面单击即开；是退出 GUI 回到文本 shell 的入口。含 `ask` 等命令。


- 标识：`Kind.Terminal` ｜ 原生 AppType：`APP_TERMINAL` ｜ 程序集：`Terminal.mex`（独立 .mex）

- **Bug（待填写）**：



### 计算器  `Kind 4`  【正常】

基础计算器窗口。


- 标识：`Kind.Calculator` ｜ 原生 AppType：`APP_CALCULATOR` ｜ 程序集：`Calculator.mex`（独立 .mex）

- **Bug（待填写）**：



### 关于  `Kind 5`  【正常】

NexOS 版本与系统信息页。


- 标识：`Kind.About` ｜ 原生 AppType：`APP_ABOUT` ｜ 程序集：`About.mex`（独立 .mex）

- **Bug（待填写）**：



### 内存优化器  `Kind 6`  【正常】

内存清理/优化小工具。


- 标识：`Kind.MemOptimizer` ｜ 原生 AppType：`APP_MEM_OPTIMIZER` ｜ 程序集：`MemOptimizer.mex`（独立 .mex）

- **Bug（待填写）**：



### 记事本  `Kind 7`  【正常】

独立 .mex 程序集，通过专属 CLR 槽加载；支持打开指定文件、保存回原卷。


- 标识：`Kind.Notepad` ｜ 原生 AppType：`APP_NOTEPAD` ｜ 程序集：`Notepad.mex`（独立 .mex）

- **Bug（待填写）**：



### 浏览器  `Kind 8`  【正常】

优先加载 WinPE 风格 PE 二进制（iexplore.exe）；若镜像缺失则回退到托管 BrowserApp，图标不会死路。


- 标识：`Kind.Browser` ｜ 原生 AppType：`APP_BROWSER` ｜ 程序集：`Browser.mex`（独立 .mex）

- **Bug（待填写）**：



### AI 一键开启向导  `Kind 9`  【正常】

一键启用 AI 的设置向导。


- 标识：`Kind.AiSetup` ｜ 原生 AppType：`APP_AISETUP` ｜ 程序集：`AiSetup.mex`（独立 .mex）

- **Bug（待填写）**：



### AI Agent  `Kind 10`  【已知限制】

Planner/Actor/Critic 架构的 AI Agent 运行器。**已知限制**：托管实现 `Shell::Open(10)` 在 CLR 中会 fault 并拖垮整个桌面渲染（黑屏），故当前**强制走原生 draw_agent 渲染**（原生实现完整）。


- 标识：`Kind.AiAgent` ｜ 原生 AppType：`APP_AIAGENT` ｜ 程序集：`AiAgent.mex`（独立 .mex）

- **Bug（待填写）**：



### 演示窗口  `Kind 11`  【正常】

按钮收缩 + 回复「啥」演示窗口。


- 标识：`Kind.Demo` ｜ 原生 AppType：`APP_DEMO` ｜ 程序集：`Demo.mex`（独立 .mex）

- **Bug（待填写）**：



### 网络工具  `Kind 12`  【正常】

网络诊断控制台（ipconfig / netstat / ping 等）。


- 标识：`Kind.NetTools` ｜ 原生 AppType：`APP_NETTOOLS` ｜ 程序集：`NetTools.mex`（独立 .mex）

- **Bug（待填写）**：



### AI 命令台  `Kind 13`  【正常】

自然语言命令控制台 / AI 启动器。


- 标识：`Kind.AiLauncher` ｜ 原生 AppType：`APP_AILAUNCHER` ｜ 程序集：`AiLauncher.mex`（独立 .mex）

- **Bug（待填写）**：



### 网络攻防模拟  `Kind 14`  【已恢复】

网络路由与攻防模拟。已从桌面/开始菜单恢复：`Listed()` 现放行 `0..18` 全部 Kind，`LoadDefaults()` 种子 19 个图标，`EnsureAllKinds()` 会补齐旧配置中缺失的 Kind。


- 标识：`Kind.NetDefense` ｜ 原生 AppType：`APP_NETDEFENSE` ｜ 程序集：`NetDefense.mex`（独立 .mex）

- **Bug（待填写）**：



### WebView（Linux VNC）  `Kind 15`  【已恢复】

Linux guest VNC 帧缓冲查看器。现已同时列入「开始 > 所有应用」与桌面图标，双击即可打开。


- 标识：`Kind.WebView` ｜ 原生 AppType：`APP_WEBVIEW` ｜ 程序集：`WebView.mex`（独立 .mex）

- **Bug（待填写）**：



### 视频播放器  `Kind 16`  【已恢复】

视频文件预览/播放。不固定到任务栏，但已在「开始 > 所有应用」与桌面图标中可达（恢复时一并种子到桌面，不再仅限开始菜单）。


- 标识：`Kind.VideoPlayer` ｜ 原生 AppType：`APP_VIDEOPLAYER` ｜ 程序集：`VideoPlayer.mex`（独立 .mex）

- **Bug（待填写）**：



### 音频播放器  `Kind 17`  【已恢复】

音频文件预览/播放。同样不固定到任务栏，但已列入「开始 > 所有应用」与桌面图标，UI 可达。


- 标识：`Kind.AudioPlayer` ｜ 原生 AppType：`APP_AUDIOPLAYER` ｜ 程序集：`AudioPlayer.mex`（独立 .mex）

- **Bug（待填写）**：



### AI 对话（AiChat）  `Kind 18`  【已修复 · 异步化】

本地 GGUF 模型对话窗口。2026-11-12 夜间起改为异步流式：点「发送」只调 `Host.AiStart(q)` 挂载解码作业并立即返回，`OnPaint()` 的 `Pump()` 每帧用 `Host.AiPoll() / AiStatus()` 拉取增量文本；生成中再点「发送」即 `Host.AiCancel()` 取消。原生侧 `ai_async_tick(3)` 每帧最多推进 3 个 transformer layer，鼠标不再卡顿；底层另有 `aiquant 0|1`、分页 KV cache 与 16KiB 分页 DMA 模型加载。  
**打不开的根因与修复**：原生 `app_for_managed_kind()` 只映射 Kind 0–13，AiChat(18) 落入 `default: APP_NONE`，双击桌面图标调用 `launch_app(APP_NONE)` 时 `launch_app_impl` 命中 `default: return`，既不创建窗口也不调用 `mforms_open` → 静默无反应。已在 `src/gui/gui_legacy.cpp` 补齐 Kind 14–18 的 AppType 枚举、双向映射与窗口尺寸分支。


- 标识：`Kind.AiChat` ｜ 原生 AppType：`APP_AICHAT` ｜ 程序集：`AiChat.mex`（独立 .mex）

- **Bug（待填写）**：



### Bug 上报（BugReport）  `Kind 19`  【本轮新增】

**把 bug 直接交给我**的专用窗口。左侧填写「出现情况 / 复现步骤 / 期望结果 / 实际结果」，点「抓取当前屏幕」调用 `Host.ApmsCapture(scale)` 把 VBE 帧缓冲降采样成 `apms_shot.ppm` 存到数据盘，点「生成报告」调 `Host.ApmsReport(text)` 写出 `apms_report.md`（含运行环境、内存堆指标、应用清单、事件环、串口日志尾部、你的描述）。之后在 Windows 侧执行 `python tools/apms_extract.py --disk build/data_vbox.vdi --out build/apms` 即可取出报告与 PNG 截图。

- 标识：`Kind.BugReport` ｜ 原生 AppType：`APP_BUGREPORT` ｜ 程序集：`BugReport.mex`（独立 .mex）

- **Bug（待填写）**：




## 系统 / 壳功能（Shell 子系统）


### 开始菜单  `Shell`  【已恢复】

置顶磁贴 + 所有应用列表 + 搜索框。磁贴可固定/取消固定，右键弹出菜单。`Listed()` 已放开为 `0..18`，`LoadInstalled()` 不再强制关掉未放行的应用，「所有应用」列表现列出**全部 19 个 Kind**。


- 入口：`Desktop.Click` 左上角 ｜ 文件：`Desktop.cs`

- **Bug（待填写）**：



### 任务栏  `Shell`  【已恢复】

置顶固定应用 + 运行中窗口按钮 + 跳转列表（Jump List，含新建窗口/取消固定/结束进程）。现默认固定 **8 个 Pin**：控制面板 / 此电脑 / 终端 / 记事本 / 浏览器 / AI Agent / AI 命令台 / AI 对话；其余 Kind 从开始菜单或桌面打开。


- 文件：`Desktop.cs` ｜ 原生：`gui_legacy.cpp`

- **Bug（待填写）**：



### 桌面图标  `Shell`  【已恢复】

图标网格、拖放排序、内联重命名、每个图标对应 .lnk 文件。双击打开（终端为单击）。`DMAX` 已从 20 提到 32，`LoadDefaults()` 种子**全部 19 个 Kind** 图标；`SyncFromFs()` 末尾的 `EnsureAllKinds()` 保证从旧配置启动时也能补齐。


- 文件：`Desktop.cs` ｜ 数据：MKFS「Desktop」文件夹

- **Bug（待填写）**：



### 锁屏 / 登录  `Shell`  【正常】

登录界面，未登录时屏蔽桌面。VM 测试通过 `Login.BypassForHost()` 自动跳过。


- 文件：`Login.cs`

- **Bug（待填写）**：



### 主题 / 个性化  `Shell`  【正常】

壁纸渐变、强调色、深色模式、像素/CRT 复古模式、桌面小组件开关、Gamma 自检。MiniCLR 不运行静态初始化器，所有字段在 `Shell.Init` 显式初始化。


- 文件：`Forms.cs`(Theme) ｜ 持久化：`nexos.cfg`

- **Bug（待填写）**：



### 语音控制  `Shell`  【正常】

主麦克风总开关；仅显式标记 `W.Voice(...)` 的控件响应。识别到的短语作为合成点击分派给匹配控件。可用 `voice on/off` 切换。


- 文件：`Voice.cs` ｜ 总开关：`Theme.VoiceOn`

- **Bug（待填写）**：



### Toast 通知  `Shell`  【正常】

瞬时通知栈（如开机「系统就绪」）。


- 文件：`Toast.cs`

- **Bug（待填写）**：



### HUD 桌面小组件  `Shell`  【正常】

时钟/负载表/火花线遥测覆盖层，每帧重绘（`Host.SetAnim(1)`）。


- 开关：`Theme.WidgetsOn` ｜ 文件：`Desktop.cs`

- **Bug（待填写）**：



### 中文输入法（IME）  `Shell`  【正常】

统一中文 IME 路由器，覆盖锁屏、内联重命名与每个 C# 文本框。


- 文件：`gfx_ime.cpp` ｜ 路由：`ime_route_global()`

- **Bug（待填写）**：



### 窗口管理  `Shell`  【正常】

打开/聚焦/最小化/关闭、动画补间（Anim）。托管窗口统一为 `APP_MANAGED`，客户区交给 C# 绘制。


- 原生：`gui_legacy.cpp` ｜ 桥：`mforms.cpp`

- **Bug（待填写）**：



### 设置持久化  `Shell`  【正常】

主题与「已安装应用」位图写入 MKFS 数据盘 `nexos.cfg` / `installed.cfg`，下次启动恢复。`LoadInstalled()` 已移除对未放行 Kind 的强制关闭循环，`SyncFromFs()` 之后由 `EnsureAllKinds()` 补齐缺失图标，旧配置不会再把 17 个应用抹掉。


- 文件：`Theme.cs` / `Desktop.cs`（Installed[]）

- **Bug（待填写）**：



### 右键 / 上下文菜单  `Shell`  【正常】

桌面图标、任务栏、窗口、托盘、Win+X 等右键菜单。含固定到开始/任务栏、打开文件所在位置等。


- 文件：`Popup.cs` / `Desktop.cs`

- **Bug（待填写）**：



### 文件拖放（跨窗口）  `Shell`  【正常】

桌面图标与资源管理器间的拖放，支持把文件拖到另一个资源管理器窗口。


- 文件：`Desktop.cs`

- **Bug（待填写）**：



## 本轮新增：异步推理与引擎加速


### 异步 AI 推理（协作式按帧分片）  `Engine`  【本轮新增】

4 个 Host icall：`AiStart / AiPoll / AiStatus / AiCancel`，状态 `0 idle · 1 running · 2 done · 3 error`。原生 `ai_async_start()` 组 ChatML prompt 后调 `qwen_gen_begin()`；`mforms_paint_overlay()` 每帧 `ai_async_tick(3)` 最多推进 3 个 transformer layer，`g_mforms_anim` 在作业运行期间保持置位。64 位内核无 IRQ / 抢占式线程调度，因此这是**协作式分片而非真线程**；每产出一个 token 让出一次，流式文本经 `ai_async_take()` 交给 C#，点击「发送」当帧即可返回。


- C#：`AiChat.cs` ｜ icall：`Forms.cs` / `mforms.cpp` ｜ 原生：`kernel64.cpp` / `gguf_infer.cpp`

- **Bug（待填写）**：



### 推理引擎加速（量化 / 分页 / DMA 加载）  `Engine`  【本轮新增】

① **W4A8/W8A8 量化**：激活按 32 元素 absmax 块量化为 int8，Q4_0/Q4_1/Q5_0/Q8_0/Q4_K/Q5_K 权重块改用 int32 整数累加再乘 scale，默认开启，`qwen_set_quant_mode(0)` 可回退浮点。② **分页 KV cache**：`KV_PAGE=16`，`kpages / vpages[layer][page]` 按需 `apalloc()`，不再一次性分配并清零约 88MB 的平坦数组。③ **更快加载**：16KiB/页 BM-DMA（64KiB 对齐 bounce、PRDT 正确 EOT 与 `0x09` 方向），首页面校验 GGUF 魔数，失败自动关 DMA + soft reset + 降级单扇区 PIO；QEMU 仍走 virtio-blk 1MiB 快路，VBox 永不使用会 busy-spin 的多扇区 PIO。


- 文件：`gguf_infer.cpp` / `gguf_infer.h` / `.attic64/kernel64.cpp` ｜ 命令：`aiquant 0|1`、`dma on|off`

- **Bug（待填写）**：


## 本轮新增：独立应用架构 + APMS 内核运维


### 独立应用（每个 .mex 单独加载）  `Arch`  【本轮新增】

**shell.mex 里已没有任何内嵌应用**：桌面 / 开始菜单 / 任务栏 / 原生启动器全部改为向内核询问应用清单。清单文件 `sfs_files/apps.man`，格式 `kind | 显示名 | mex 文件 | 图标色 | 字母 | 说明`，一行一个应用，`#` 开头为注释。内核启动时 `sfs.read("apps.man")` 解析后填入 `g_apms_apps[]`（上限 32 个）。新增一个应用 = 放一个 `.mex` + 写一行清单，无需改内核或壳代码。

- 清单：`sfs_files/apps.man` ｜ 解析：`.attic64/apms.inc` ｜ 加载：`mforms.cpp`

- **Bug（待填写）**：




### 独立 CLR 槽（应用之间互不污染）  `Arch`  【本轮新增】

每个独立应用窗口分配一个专属 CLR slot（slot 0 = 壳，slot 1..11 = 独立应用），拥有独立的 image / heap / 静态字段；窗口关闭时 `clr_close_slot(slot)` 释放约 840 KiB。窗口句柄从 `NEX_APP_HANDLE_BASE = 64` 起，与壳管理的 0..15 app id 完全隔离；`mforms_drag` / `mforms_rclick` 对独立句柄直接 opt-out，避免调用应用里不存在的 `Shell::WindowDrag` / `Shell::RightClick`。

- 文件：`clr.h` / `clr.cpp` / `mforms.cpp`

- **Bug（待填写）**：




### APMS 串口日志环（8 KiB tee）  `APMS`  【本轮新增】

所有 `serial_puts()` / `serial_tx()` 逐字节 tee 进 8 KiB 环形缓冲；CLR fault 因直写 UART 不经过 `serial_puts`，故由 `apms_on_clr_fault()` 显式补录。文本 shell 命令 `apms log` 可打印尾部，托管侧 `Host.ApmsLogTail()` 同源读取。

- 文件：`.attic64/apms.inc` ｜ 内核：`.attic64/kernel64.cpp`

- **Bug（待填写）**：




### APMS 结构化事件环  `APMS`  【本轮新增】

64 条 `ApmsEvent{seq, stamp, code, tag[12], msg[84]}` 环形记录，覆盖：启动步进（boot step）、CPU fault、CLR fault、应用启动（app-launch）、截图（capture）、报告（report）。`apms events` 打印，托管侧 `Host.ApmsEvent(idx)` 逐条取回，报告里会整段带上。

- 文件：`.attic64/apms.inc` ｜ 命令：`apms status / apps / events / log / clear`

- **Bug（待填写）**：




### APMS 屏幕截图  `APMS`  【本轮新增】

读 VBE info block `0x5000`（fb 物理地址、宽高、pitch、像素格式），支持 BGRX32 / RGBX32 / RGB24 / RGB565 四种格式；**先把帧缓冲读进 RAM 再写盘**（规避 >4 GiB LFB 的 heisenbug），按 scale 降采样后写成 P6 PPM，落到 MKFS 数据盘 `apms_shot.ppm`。

- 文件：`.attic64/apms.inc` ｜ 命令：`apms shot [1..8]` ｜ icall：`Host.ApmsCapture(scale)`

- **Bug（待填写）**：




### APMS 报告生成与提取  `APMS`  【本轮新增】

报告为 Markdown，含运行环境、内存堆指标、应用清单、事件环、串口日志尾部、截图说明，以及你填写的出现情况与复现步骤。Windows 侧用 `python tools/apms_extract.py --disk build/data_vbox.vdi --out build/apms` 解析 MKFS 超级块与文件表，取出 `apms_report.md`，并把 `apms_shot.ppm` 转换成可直接附图的 `apms_shot.png`。

- 内核：`apms_report(user_text)` → `apms_report.md` ｜ 工具：`tools/apms_extract.py`

- **Bug（待填写）**：

