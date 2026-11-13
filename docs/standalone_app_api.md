# NexOS 独立应用开发 API 文档

> 面向「要修改 / 新增 NexOS 独立应用」的开发者。
> 独立应用 = 一个独立的 `.mex` 程序集，运行在自己的 MiniCLR slot 里，与桌面壳完全解耦。
> 版本：2026-11-12

---

## 0. 一句话总览

| 你要做的事 | 改哪里 |
|---|---|
| 改某个已存在应用的界面/逻辑 | `csharp/apps/standalone/<Name>/`（源码多为 `parts/*.cs` 或 `csharp/apps/Shell/*.cs`） |
| 新增一个全新应用 | 见 §6「新增应用四步」 |
| 改通用控件（按钮/输入框/绘制） | `csharp/NexOS.Forms/Forms.cs` |
| 改内核侧窗口调度 | `mforms.cpp`（`slot_symbol()` / `mforms_open_standalone()`） |
| 改应用清单（名字/图标/说明） | `sfs_files/apps.man` |
| 改构建流水线 | `tools/gen_standalone_apps.py` → 生成 `tools/standalone_apps.mk` |

---

## 1. 代码在哪（目录地图）

```
E:\NexOS\NexOS\
├─ csharp/
│  ├─ NexOS.Core/            ← 迷你 BCL，**所有应用必链**
│  │  ├─ Corelib.cs          System.Object/String/Array 等替身
│  │  ├─ Sys.cs              NexOS.Sys：内核字符串/时钟原语（icall）
│  │  └─ SystemAlgo.cs       空间索引/A*/LRU/Bloom 等算法
│  ├─ NexOS.Forms/           ← **UI 工具箱，所有应用必链**
│  │  ├─ Forms.cs            Gfx / Host / W / C / Theme / U / TBox / App / VK / WAct
│  │  ├─ Anim.cs             动画补间（Hover/Press/Toggle 状态）
│  │  └─ Voice.cs            语音绑定（仅壳侧，独立应用自动禁用）
│  ├─ apps/
│  │  ├─ standalone/          ← ★ 独立应用工程区
│  │  │  ├─ Shared/Shim.cs    兼容垫片：Kind / Desktop / Shell / Popup 的独立实现
│  │  │  ├─ parts/            被拆分的应用实现（见下表）
│  │  │  ├─ BugReport/        ★ 唯一自带 Program.cs 的应用（参考模板）
│  │  │  └─ <Name>/           每个应用一个目录：App.csproj + Main.cs
│  │  ├─ Shell/               旧位置，部分应用源码仍在这里
│  │  │  └─ TerminalApp.cs / Browser.cs / AiChat.cs / AiAgent.cs ...
│  │  └─ mex/Notepad/         ★ Notepad 参考实现（自带 csproj，最简洁）
│  └─ ...
├─ sfs_files/
│  ├─ apps.man                ← ★ 运行时唯一真源：应用清单
│  └─ *.mex                   构建产物（勿手改）
├─ tools/
│  ├─ gen_standalone_apps.py 生成每个应用的 csproj / Main.cs / standalone_apps.mk
│  ├─ split_app_sources.py   把 Apps.cs 拆成 parts/*.cs（省 SFS 空间）
│  ├─ mex_pack.py             Roslyn DLL → .mex 平面镜像
│  └─ apms_extract.py         从数据盘抽出 APMS 报告与截图
├─ mforms.cpp                 内核侧托管窗口桥（icall 注册表 + 独立 slot 调度）
├─ Makefile                   主构建（含 Notepad / shell.mex 规则）
└─ docs/standalone_app_api.md  ← 本文档
```

### 应用源码归属速查

| 应用 | mex | 源码文件 |
|---|---|---|
| Settings | ControlPanel.mex | `standalone/parts/ControlPanelApp.cs` |
| This PC | FileExplorer.mex | `standalone/parts/FileExplorerApp.cs` |
| Task Mgr | TaskManager.mex | `standalone/parts/TaskManagerApp.cs` |
| Terminal | Terminal.mex | `Shell/TerminalApp.cs` |
| Calculator | Calculator.mex | `standalone/parts/CalculatorApp.cs` |
| About | About.mex | `standalone/parts/AboutApp.cs` |
| Optimizer | MemOptimizer.mex | `standalone/parts/MemOptimizerApp.cs` |
| Notepad | Notepad.mex | `mex/Notepad/Program.cs`（自带工程） |
| Browser | Browser.mex | `Shell/Browser.cs` |
| AI Setup | AiSetup.mex | `Shell/AiSetup.cs` |
| AI Agent | AiAgent.mex | `Shell/AiAgent.cs` |
| Demo | Demo.mex | `Shell/Demo.cs` |
| Net Tools | NetTools.mex | `Shell/NetTools.cs` |
| AI 命令台 | AiLauncher.mex | `Shell/AiLauncher.cs` |
| NetDefense | NetDefense.mex | `Shell/NetDefenseApp.cs` |
| WebView | WebView.mex | `Shell/WebView.cs` |
| Video Player | VideoPlayer.mex | `standalone/parts/VideoPlayerApp.cs` |
| Audio Player | AudioPlayer.mex | `standalone/parts/AudioPlayerApp.cs` |
| AI 对话 | AiChat.mex | `Shell/AiChat.cs` |
| Bug 上报 | BugReport.mex | `standalone/BugReport/Program.cs`（自带工程） |
| AI 工作台 | AiWorkbench.mex | `standalone/parts/AiWorkbenchApp.cs` |

---

## 2. 生命周期与入口契约

内核（`mforms.cpp::slot_symbol()`）把壳符号 `NexOS.Forms.Shell::<M>` 在 slot > 0 时重写为 `App.Main::<M>`。

**独立应用只需暴露 5 个静态方法**：

```csharp
namespace App
{
    public static class Main
    {
        public static void  Init();                 // slot 装载时调用一次
        public static int   Paint(int id, int w, int h);   // 每帧绘制
        public static int   Click(int id, int x, int y);   // 鼠标点击（窗口内坐标）
        public static int   Key(int id, int ch);           // 键盘（ch 见 §7）
        public static string Title(int id);                // 窗口标题
    }
}
```

### 重要限制

| 能力 | 独立应用 | 说明 |
|---|---|---|
| `Paint` / `Click` / `Key` / `Title` / `Init` | ✅ | 全部支持 |
| 右键菜单 `OnRightClick` | ❌ | 内核对独立窗口直接返回 0（避免 MiniCLR 未解析方法 fault） |
| 窗口拖拽 `OnDrag` | ❌ | 同上，独立窗口主动 opt-out |
| 文件拖放 `AcceptDrop` | ❌ | 同上 |
| 语音绑定 `W.Voice()` | ❌ | Voice.cs 是壳侧代码，`STANDALONE_APP` 下自动空实现 |

> 如需扩展契约，改 `mforms.cpp` 中 `nex_handle_index(id) >= 0` 的几处早退分支，并在 `App.Main` 里补对应方法。

---

## 3. `App` 基类

继承 `NexOS.Forms.App`，重写需要的虚方法。

```csharp
using NexOS.Forms;

namespace App
{
    public class MyApp : NexOS.Forms.App
    {
        public override void OnPaint() { }
        public override void OnClick(int mx, int my) { }      // mx,my = 窗口内坐标
        public override void OnKey(int ch) { }
        public override void OnMouseDown(int btn, int mx, int my) { }
        public override void OnMouseUp(int btn, int mx, int my) { }
        public override void OnMouseMove(int mx, int my) { }
        public override void OnWheel(int dy) { }
        public override string GetTitle() { return "我的应用"; }
    }
}
```

### 成员

| 成员 | 说明 |
|---|---|
| `int id` | 窗口句柄，由内核在每次回调前写入 |
| `int KindId` | 启动 kind（对应 apps.man 的 kind 列），由 `Main.Init()` 设定 |
| `static App Current` | 当前应用实例。`W.*` 控件把命中矩形注册到它上面，**必须**在 Paint 前赋值 |
| `RegisterHit(id,x,y,w,h)` | 手动注册命中矩形（上限 64 个/帧） |
| `hitId/hitX/hitY/hitW/hitH/hitN` | 上一帧的命中矩形表 |

### 生命周期铁律

> **MiniCLR 不运行静态字段初始化器（`.cctor`）。**
> 所有 `static` 字段必须在 `Init()` 或构造函数里 **显式赋值**，否则永远是 0/null。
> 这也是 `Main.Init()` 里必须调用 `Anim.Init()` 的原因。

---

## 4. 绘制 API — `Gfx`

坐标原点在窗口左上角，单位像素。颜色是 `0xRRGGBB`（uint）。

```csharp
Gfx.FillRect(x, y, w, h, c);
Gfx.FillRound(x, y, w, h, radius, c);
Gfx.DrawRound(x, y, w, h, radius, c);
Gfx.DrawRect(x, y, w, h, c);
Gfx.DrawLine(x0, y0, x1, y1, c);
Gfx.Gradient(x, y, w, h, topColor, bottomColor);
Gfx.Text(x, y, s, fg);
Gfx.TextBg(x, y, s, fg, bg);
Gfx.TextCenter(x, y, w, s, fg);      // w 为一段文字的宽度盒
Gfx.FillCircle(cx, cy, r, c);
Gfx.DrawCircle(cx, cy, r, c);
Gfx.Icon(x, y, size, bg, letter, letterColor);
Gfx.Progress(x, y, w, h, pct, c);
Gfx.Glass(x, y, w, h, radius, tint, alpha, blur);
Gfx.TextPx(x, y, s, fg, px);         // 指定字号绘制
Gfx.MeasurePx(s, px);                // 量取指定字号宽度
Gfx.Measure(s);                      // 默认字号宽度
```

### 尺寸与鼠标

```csharp
int  Gfx.Width();      // 当前窗口客户区宽
int  Gfx.Height();     // 当前窗口客户区高
int  Gfx.ScreenW();    // 屏幕宽
int  Gfx.ScreenH();    // 屏幕高
int  Gfx.MouseX();     // 鼠标 X（窗口内坐标）
int  Gfx.MouseY();
int  Gfx.OriginX();    // 窗口在屏幕上的原点 X
int  Gfx.OriginY();
void Gfx.SetMouse(x, y);
```

### 图片

```csharp
int  Gfx.HasImage(id);   // 该纹理是否存在
void Gfx.Image(id, x, y, w, h);
```

纹理 id（`Tex` 常量）：

| 常量 | 值 | 含义 |
|---|---|---|
| `Tex.Wall` | 0 | 壁纸 |
| `Tex.Task` | 1 | 任务栏 |
| `Tex.Menu` | 2 | 菜单 |
| `Tex.Chrome` | 3 | 窗口边框 |
| `Tex.WinBg` | 4 | 窗口背景（推荐用作 `W.Clear()` 底） |
| `Tex.WallDyn + i` | 10+i | 用户壁纸，i ∈ [0, Host.WallpaperCount()) |
| `Tex.Icon + Kind` | 100+kind | 应用图标 |

---

## 5. 控件 API — `W`（即时模式）

即时模式：每次 `OnPaint()` 重新画全部控件，**不保留状态**。状态由你自己存在实例字段里，命中判定自己写。

```csharp
W.Clear();                                   // 铺底（自动处理滚动偏移）
W.Card(x, y, w, h);                          // 圆角卡片 + 描边
W.Panel(x, y, w, h, bg);
W.Label(x, y, s);                            // 主文本
W.Sub(x, y, s);                              // 次文本
W.Header(x, y, s);                           // 带蓝色竖条的标题
W.Primary(x, y, w, h, "确定");                // 实心主按钮
W.Button(x, y, w, h, "取消");                 // 描边次按钮
W.Key(x, y, w, h, "7", accent:false);        // 大号按键
W.Row(x, y, w, text, selected);              // 列表行
W.Meter(x, y, w, "内存", pct, fillColor);     // 进度条 + 标签
```

### 交互与动画

```csharp
bool W.Hot(x, y, w, h);      // 鼠标在矩形内
int  W.Hover(x, y, w, h);    // 动画化 hover 0..1000
```

### 状态控件（返回 1 表示鼠标悬停，状态由调用方持有）

```csharp
int  W.Toggle(x, y, w, h, on, id);           // 开关
int  W.Check(x, y, size, on, id);            // 复选框
int  W.Radio(x, y, size, on, id);            // 单选钮
int  W.Slider(x, y, w, h, pct, id);          // 滑块
int  W.Progress(x, y, w, h, pct, color, id); // 进度条（带动画）
```

> `id` 必须是**稳定唯一**的整数（用作动画 key），固定布局可用 `Anim.Key(x,y,w,h)`。

### 语音（独立应用内为空实现）

```csharp
W.Voice("打开设置", x, y, w, h);   // 独立应用无效，壳内有效
```

---

## 6. 颜色与主题

### `C` — WinUI 3 标准色板（只读常量）

```csharp
C.WinBg      // 窗口背景
C.Card       // 卡片面
C.CardAlt    // 交替行
C.Border     // 发丝线
C.BorderMid  // 中等描边
C.Accent     // 强调蓝
C.AccentHi   // hover 强调
C.AccentLo   // 按下强调
C.AccentText // 强调色文字
C.OnLight    // 浅色面上的文字
C.Text       // 主文本
C.TextSub    // 次文本
C.TextFaint  // 三级/禁用
C.White
C.Hover      // 控件 hover
C.Sel        // 选中行
C.Good / C.Warn / C.Danger
```

### `Theme` — 可变设计令牌（静态字段，需显式赋值）

```csharp
Theme.Accent / Theme.Dark / Theme.WallIdx / Theme.DesktopMode
Theme.PixelMode / Theme.PixelScale / Theme.PixelScan
Theme.TextGamma
```

---

## 7. 工具类 `U` / `Sys` / `Anim`

### `U`

```csharp
string U.I(int v);                       // int → string
string U.Cat(a, b, c, ...);              // 字符串拼接（最多 7 参）
string U.Sub(s, start, len);             // 子串（UTF-8 字节安全，O(n) 一次分配）
string U.Flat(s);                        // 压平换行
bool   U.In(mx, my, x, y, w, h);         // 点在矩形内
bool   U.IsExe(name);
uint   U.LerpColor(c0, c1, t);           // t ∈ 0..1000
uint   U.Alpha(c, a);                    // a ∈ 0..255
uint   U.Fade(c, t);
uint   U.Shade(c, d);                    // 亮/暗
string U.Mb(kb);
```

> ⚠️ **永远不要**写 `for(...) r = U.Cat(r, Host.CharStr(s[i]))` 这种 O(n²) 拼接 —— MiniCLR 堆是 bump 分配器，会直接耗尽并 fault。用 `U.Sub` / `U.Cat`。

### `NexOS.Sys`（icall）

```csharp
Sys.Print(s); Sys.PrintInt(v); Sys.PrintChar(c);
Sys.StrConcat / StrCharAt / StrLen / StrSub / StrFlat / StrEq
Sys.IntToStr(v); Sys.TickCount();
```

### `Anim`

```csharp
Anim.Init();                 // ★ 必须在 Main.Init() 里调用
Anim.Set(key, target, durMs, easing);
Anim.Get(key); Anim.ToOf(key); Anim.Snap(key, value);
Anim.Hover(key, hot); Anim.Press(key, pressing);
Anim.Key(x, y, w, h);        // 坐标 → 稳定 key
Anim.Tick();                  // 内核每帧调用，一般不需要手动调
```

---

## 8. 输入 — `TBox` 与 `VK`

### `TBox`（多行文本编辑器模型）

```csharp
TBox box = new TBox();
box.text = "初始内容";
box.cursor = 0; box.selA = 0; box.selB = 0;

bool box.Key(int ch);        // ★ 把按键交给它处理，返回 true = 已消费
```

编辑：

```csharp
box.Insert(s); box.Backspace(); box.Delete();
box.Undo(); box.Redo();
string box.GetSelection();
box.SelectAll();
```

移动：

```csharp
box.MoveLeft(); box.MoveRight(); box.Home(); box.End();
box.MoveHome(); box.MoveEnd();
box.MoveVertical(delta);     // ↑↓，保持列号
box.ExtendVertical(delta);   // Shift+↑↓
```

行列查询：

```csharp
int  box.LineOf(pos);        // pos 在第几行
int  box.ColOf(pos);
int  box.LineCount();
int  box.LineStart(i);       // 第 i 行起始字节偏移
```

### `VK`（虚拟键常量）

```csharp
VK.Back=8  VK.Enter=13  VK.Esc=27  VK.Delete=-26
VK.CtrlC=-3 VK.CtrlV=-4 VK.CtrlZ=-5 VK.CtrlA=-6 VK.CtrlE=-7
VK.CtrlU=-8 VK.CtrlK=-9 VK.CtrlW=-10 VK.CtrlL=-11 VK.CtrlD=-12
VK.CtrlR=-13 VK.AltF=-15 VK.AltB=-16 VK.Tab=-17
VK.Up=-18 VK.Down=-19 VK.Left=-20 VK.Right=-21
VK.HomeK=-22 VK.EndK=-23 VK.PageUp=-24 VK.PageDown=-25
VK.F1..F12 = -40..-51
VK.CtrlPlus=-60 VK.CtrlMinus=-61 VK.Ctrl0=-62
```

> 普通字符传入的是 **codepoint（≥32）**；退格是 `-1` 或 `8`。

---

## 9. 内核服务 — `Host`

全部是 icall，由 `mforms.cpp` 的注册表分派。独立应用可直接调用。

### 系统信息

```csharp
Host.MemTotalKb(); Host.PagesFree(); Host.PagesUsed(); Host.PagesTotal();
Host.HeapAlloc(); Host.HeapFree(); Host.HeapAllocCnt(); Host.HeapFreeCnt();
Host.Optimize();
Host.Hour(); Host.Minute(); Host.Second(); Host.Ticks(); Host.TickMs();
Host.OsName(); Host.CpuVendor(); Host.DiskModel(); Host.DiskSizeMb();
Host.Is64Bit(); Host.PciCount(); Host.NicPresent(); Host.CpuLoad();
```

### 文件系统（fs = 卷索引）

```csharp
Host.FileCount(fs); Host.FileName(fs, idx); Host.FileIsDir(fs, idx);
Host.EnterDir(fs, idx); Host.DirUp(fs); Host.CwdPath(fs);
Host.EnterPath(fs, path);
Host.FileSize(fs, name); Host.IsDirByName(fs, name);
Host.ReadText(fs, name); Host.ReadHex(fs, name);
Host.WriteText(fs, name, text);            // 返回写入字节数
Host.FileMkDir(fs, name); Host.FileDelete(fs, name); Host.FileRename(fs, old, nw);
Host.CopyRange(srcFs, srcPath, name, dstFs, dstPath, off, len);
Host.FileRefresh();
```

### 进程 / 窗口

```csharp
Host.Exec(cmd);            // 执行内核命令，返回输出
Host.RunExe(name);         // 运行 PE 可执行
Host.Shutdown(); Host.Reboot();
Host.Log(s);               // 写入串口日志（APMS 会收）
Host.OpenApp(kind); Host.OpenAppNew(kind); Host.CloseApp(kind);
Host.ActivateManaged(mid);
Host.WinAction(id, WAct.Close);   // WAct: Restore10 Move11 Size12 Min13 Max14 Refresh15 Close17
Host.ExitGui();
Host.GetClipboard(); Host.SetClipboard(s);
Host.CharStr(ch);
```

### 网络

```csharp
string Host.HttpGet(url);
int    Host.DownloadFile(url, dest, timeoutMs);
int    Host.DownloadAsync(url, dest);
string Host.DownloadStatus(dest);
int    Host.DownloadCancel(dest);
```

### AI 推理（本地 GGUF 模型，异步）

```csharp
int    Host.AiStart(prompt);   // 启动生成，立即返回
string Host.AiPoll();          // 取已生成文本片段
int    Host.AiStatus();        // 0 空闲 / 1 运行中 / 2 完成 / 3 出错
void   Host.AiCancel();
```

> 流式用法：`AiStart()` 后在 `OnPaint()` 里反复 `AiPoll()` 取增量并追加到显示缓冲。

### 音频

```csharp
int    Host.AudioAvailable();   // 先判断，否则播放必然失败
string Host.AudioInfo();
int    Host.AudioPlay(pcmBytes, nframes);
```

### 应用清单（读 apps.man）

```csharp
int    Host.AppCount();
int    Host.AppKind(idx);
string Host.AppName(idx);
string Host.AppMex(idx);
int    Host.AppColor(idx);
int    Host.AppLetter(idx);
string Host.AppDesc(idx);
int    Host.AppKindByMex(mex);
```

### APMS 运维（Bug 上报）

```csharp
int    Host.ApmsStatus();       // 事件条数
string Host.ApmsEvent(idx);     // 第 idx 条事件
string Host.ApmsLogTail();      // 串口日志尾部
int    Host.ApmsCapture(scale); // 截图写 apms_shot.ppm，返回字节数
int    Host.ApmsReport(text);   // 生成 apms_report.md，返回字节数
int    Host.ApmsClear();
```

---

## 10. 兼容垫片 `Shim.cs`

独立应用**不能**引用 `Desktop.cs` / `Shell.cs` / `Popup.cs`。`Shared/Shim.cs` 提供同名替身：

```csharp
Kind.ControlPanel=0 ... Kind.AiWorkbench=20;     // 与 apps.man 的 kind 一致
Desktop.KindCount / KindName(k) / AppColor(k) / AppLetter(k) / AppDesc(k)
Desktop.OpenFileMenu(...) / OpenFolderMenu(...)  // 空实现
Shell.Open(kind) / Close(id) / OpenNotepadFs(...) / ById(id)→null
Popup.Open(...) / Close()                         // 空实现
```

---

## 11. 编译限制（MiniCLR / Roslyn `/nostdlib`）

| 限制 | 说明 |
|---|---|
| 无 `System.*` BCL | 只有 `Corelib.cs` 里那几个替身：`Object/String/Array/Int32/...` |
| 无泛型 | `List<T>` / `Dictionary<K,V>` **不可用**，用数组 |
| 无 `System.Text.StringBuilder` | 用 `U.Cat` / `U.Sub` |
| 无 `string.Format` | 用 `U.Cat(a, U.I(x), b)` |
| 无 LINQ | 手写循环 |
| 无 `string.EndsWith/ToLower/StartsWith` | 用 `U.IsExe` 或手写字符比较 |
| 无异常处理 | `throw` 会 fault；**不要 try/catch** |
| 无静态初始化器 | 静态字段必须显式赋值 |
| 字符串是 UTF-8 **字节数组** | `s.Length` 是字节数，不是字符数；`U.Sub` 字节安全 |
| 堆是 bump 分配器 | Paint 每帧会 rewind 堆；**不要把临时对象存到实例字段**（会漏） |

### 线程/异步

64 位内核**没有 IRQ、没有抢占线程**，AI 生成是**协作式按帧分片**（每帧最多 3 层）。
所以「异步」= 在 `OnPaint()` 里轮询 `Host.AiPoll()`，而不是开线程。

---

## 12. 新增一个应用（四步）

### 第 1 步：写源码

最简模板（放 `csharp/apps/standalone/MyApp/MyApp.cs`，或直接加进 `parts/`）：

```csharp
using NexOS.Forms;

namespace App
{
    public class MyApp : NexOS.Forms.App
    {
        int count;

        public MyApp() { count = 0; }          // ★ 构造函数里显式初始化静态/实例状态

        public override void OnPaint()
        {
            W.Clear();
            W.Header(16, 16, "我的应用");
            W.Label(16, 48, U.Cat("点击次数：", U.I(count)));
            W.Primary(16, 80, 160, 38, "加一");
        }

        public override void OnClick(int mx, int my)
        {
            if (U.In(mx, my, 16, 80, 160, 38)) count++;
        }

        public override string GetTitle() { return "我的应用"; }
    }
}
```

### 第 2 步：注册进生成器

编辑 `tools/gen_standalone_apps.py` 的 `APPS` 列表：

```python
APPS = [
    ...
    ("MyApp", f"{STAND}/parts/MyAppApp.cs", "MyApp", False),
]
```

并把新 kind 加进 `KIND_OF`：

```python
KIND_OF = { ..., "MyApp": 20 }
```

然后运行：

```bash
python tools/split_app_sources.py && python tools/gen_standalone_apps.py
```

> 生成器会自动产出 `csharp/apps/standalone/MyApp/App.csproj` + `Main.cs`，
> 以及 `tools/standalone_apps.mk` 里的构建规则。
> **改完源码后不必重跑生成器**（除非改了 APPS 列表）。

### 第 3 步：写清单

在 `sfs_files/apps.man` 末尾加一行：

```
20|我的应用|MyApp.mex|2D7FF9|M|一句话说明
```

格式：`kind | 显示名 | mex 文件 | 图标色(RGB hex) | 字母 | 说明`

- kind **不可重编已有 id**
- mex 文件名 **≤ 19 字符**，必须放在 `sfs_files/`
- `#` 开头或空行被忽略

### 第 4 步：编译 + 同步

```bash
vm-run "make build/os_v2.img MODEL_GGUF=build/qwen2-0_5b-instruct-q4_k_m.gguf"
powershell -ExecutionPolicy Bypass -File tools\vbox_sync.ps1
```

> ⚠️ **不重新同步 VDI 就会看到旧镜像**。
> ⚠️ SFS 总容量 ≈ 9.96 MB；每个新 `.mex` 约 35–60 KB。

---

## 13. 修改已有应用的实操流程

```
1. 改源码（parts/*.cs 或 Shell/*.cs）
2. vm-run "make build/os_v2.img MODEL_GGUF=build/qwen2-0_5b-instruct-q4_k_m.gguf"
3. powershell -ExecutionPolicy Bypass -File tools\vbox_sync.ps1
```

**如果改的是 `Forms.cs` / `Anim.cs` / `Corelib.cs` / `Shim.cs`**（所有应用共享），
每个 `.mex` 都会重编，SFS 会明显变大 —— 注意别超容量。

**如果改的是清单 `apps.man`**：清单打进 SFS，同样需要走第 2、3 步。

---

## 14. 排错速查

| 症状 | 原因 |
|---|---|
| 应用点了没反应 | `Main.Init()` 里没设 `App.Current = app;` 或没调 `Anim.Init()` |
| 启动即 fault | 静态字段未显式初始化；或引用了壳侧类型（`Desktop`/`Shell`/`Popup` 未用 Shim） |
| 编译报 `CS0234` | 用了不存在的命名空间（如 `NexOS.Net`，那只在 Windows harness 存在） |
| 画面花屏 / 紫色竖条 | 帧缓冲 bpp 与内核预期不符（`stage2.asm` 选模式 + `0x500F` pixel_format） |
| 中文变成乱码 | 用了 `string.Length` 当字符数；或用 `U.Cat` 逐字节拼 UTF-8 |
| 内存爆掉 | Paint 里把对象存进实例字段（堆 rewind 后指针悬空）；或 O(n²) 字符串拼接 |
| 应用不出现 | `apps.man` 没加行 / kind 重复 / mex 名 > 19 字符 / 没重新编译 |
| 独立 slot 不足 | `CLR_MAX_VMS = 12`（slot 0 = 壳，1..11 = 独立应用），同屏最多 11 个独立窗口 |

---

## 15. 提交 Bug 的标准流程（APMS）

1. 打开「Bug 上报」应用（BugReport.mex）
2. 在文本框里写清 **出现情况 / 复现步骤 / 期望结果 / 实际结果**
3. 点「抓取当前屏幕」→ 写 `apms_shot.ppm`
4. 点「生成报告」→ 写 `apms_report.md`
5. 宿主机抽取：

```bash
python tools/apms_extract.py --disk build/data_vbox.vdi --out build/apms
```

产出 `apms_report.md` + `apms_shot.png`，把两者交给助手即可。
