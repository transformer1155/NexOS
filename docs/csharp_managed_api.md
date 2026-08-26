# NexOS.Forms — C# 托管层 API 文档

> 事实来源：`gui.cpp`（绘图原语 / 像素后处理）、`mforms.cpp`（InternalCall 绑定与文件操作实现）、`mforms.h`（宿主回调结构）、`csharp/NexOS.Forms/Forms.cs`、`csharp/NexOS.Core/Sys.cs`、`csharp/winhost/Backend.cs`（Windows 宿主同签名的真实实现）。
>
> 适用版本：NexOS MiniCLR + NexOS.Forms（Win11 风格托管 Shell）。

---

## 1. 架构总览

NexOS 的图形界面分两层：

- **托管层（C#）**：所有窗口、控件、应用都是 C# 代码（`NexOS.Forms` 命名空间），由 MiniCLR 解释执行，编译进 `shell.mex` → `sfs.img` → `os.img`。
- **原生层（C++）**：内核持有帧缓冲（framebuffer）与输入。`gui.cpp` 提供 `Graphics` 绘图原语与像素后处理；`mforms.cpp` 是托管壳的“原生宿主”，它做两件事：
  1. 把 `NexOS.Forms.Gfx` / `NexOS.Forms.Host` / `NexOS.Sys` 注册为 **internal call**，让 C# 能画图、读机器状态、操作文件；
  2. 暴露一组 `mforms_*` C 入口供 `gui.cpp` 调用，用来绘制一个托管窗口、路由一次点击/按键。

```
  ┌─────────────────────────────────────────────┐
  │  C# managed (shell.mex)                       │
  │   NexOS.Forms.Gfx / .Host  → [InternalCall]  │
  │   NexOS.Sys (Print/字符串/时间)              │
  └──────────────┬──────────────────────────────┘
                 │  icall 表 (mforms.cpp g_regs[])
  ┌──────────────┴──────────────────────────────┐
  │  mforms.cpp  (native host)                   │
  │   g_* 绘图包装 (加窗口原点/裁剪)            │
  │   h_* 文件/机器操作 → MFormsHost 回调表     │
  └──────────────┬──────────────────────────────┘
                 │  MFormsHost 函数表 (启动时由 gui.cpp 填充)
  ┌──────────────┴──────────────────────────────┐
  │  gui.cpp  (Graphics / 帧缓冲 / 输入)        │
  │   fill_rect / draw_text_utf8 / pixelate...  │
  └─────────────────────────────────────────────┘
```

**InternalCall 绑定机制**：`mforms.cpp::g_regs[]` 以 `"NexOS.Forms.Gfx::FillRect"` 这样的全限定名映射到原生函数 `g_fill_rect`。`mforms_init()` 通过 `clr_register_icall()` 注册整张表，必须在 `mforms_start()`（加载 `shell.mex` + 调 `Shell::Init`）之前完成。`tools/mex_pack.py` 记录 mex 中的 internal-call 名称，C# 侧与方法名必须保持一致。

---

## 2. 关键约束（写托管代码前必读）

| 约束 | 说明 |
|------|------|
| **坐标系** | C# 始终用 **客户端像素**，原点在窗口内容区左上角。原生侧（`g_fill_rect` 等）自动加窗口原点 `g_ox/g_oy` 并裁剪到客户矩形——错误的 C# 应用只会画脏自己的窗口，不会溢出到标题栏/任务栏/邻窗。 |
| **绝对屏幕坐标** | 菜单定位、右键、鼠标命中用 `Gfx.ScreenW/ScreenH/MouseX/MouseY`，**不要**用 `Gfx.Width/Height`（窗口内时只反映客户区大小，弹菜单会错位）。 |
| **bump 堆** | CLR 堆是 bump 分配器，每帧 Paint 后回卷（watermark）。**Paint() 中分配的对象不保留**；持久状态必须在 `Init/Open/Click/Key` 中创建并存入字段。 |
| **数组** | 数组是 4 字节槽位 → 只用 `int[]` 和引用 `[]`。 |
| **禁用的 BCL 特性** | 无浮点、无泛型、无接口、无 try/catch、无 `string.Format`。`string` 仅提供 `Length`、`[i]`、`Concat`。静态字段初始器**不运行**（无 `.cctor`），静态量要在 `Init()` 显式赋值。 |
| **字符串拼接** | 用 `U.Cat` / `Sys.StrConcat` / `Sys.StrSub` / `Sys.StrFlat`，**禁止**“逐字符 `r = Cat(r, CharStr(s[i]))`”循环——在 bump 堆上是 O(n²) 字节，曾直接耗尽堆导致整个托管壳静默退役。 |
| **fs 编号** | `0` = MKFS 用户盘（可写），`1` = SFS 系统盘（只读），`3` = MKFS/Desktop 下的桌面快捷方式目录；`2` 保留（FAT32）。 |
| **含空格文件名** | `h_file_name` 现已以 SFS 的 ` (NNNB)` 注解为唯一边界，裸名延伸到行尾——含空格名（`This PC.lnk`、`New File.txt`）能正确返回。旧逻辑遇空格截断，会使删/开/重命名静默 no-op（已修复）。 |

---

## 3. `NexOS.Forms.Gfx` — 绘图 API

所有方法为 `public static extern`，坐标传入**客户端像素**（原生侧加窗口原点并裁剪）。颜色为 `0xRRGGBB`（24 位）。

| C# 方法 | 原生实现 | 行为 / 备注 |
|---------|----------|-------------|
| `FillRect(x,y,w,h,c)` | `g_fill_rect` → `host.fill_rect` | 填充矩形，经 `clip_rect` 裁剪。 |
| `FillRound(x,y,w,h,r,c)` | `g_fill_round` → `host.fill_round` | 圆角填充；完全在客户区内走 `fill_round`，部分出界则降级为 `fill_rect`（保像素、丢圆角）。 |
| `DrawRound(x,y,w,h,r,c)` | `g_draw_round` → `host.draw_round` | 圆角描边（仅在完全可见时绘制）。 |
| `DrawRect(x,y,w,h,c)` | `g_draw_rect` → `host.draw_rect` | 矩形描边（仅在完全可见时绘制）。 |
| `DrawLine(x0,y0,x1,y1,c)` | `g_draw_line` → `host.draw_line` | 直线；轴对齐段被精确裁剪，其它段仅在完全可见时绘制。 |
| `Gradient(x,y,w,h,top,bot)` | `g_fill_grad` → `host.fill_grad` | 竖直渐变。 |
| `Text(x,y,s,fg)` | `g_text` → `host.text` | 单行文本（UTF-8，CJK 16px）。原生侧 `clip_text` 做裁剪，多行调用者需自行分行。 |
| `TextBg(x,y,s,fg,bg)` | `g_text_bg` → `host.text_bg` | 带背景色文本。 |
| `TextCenter(x,y,w,s,fg)` | `g_text_center` → `host` | 在 `w` 宽度内水平居中文本。 |
| `FillCircle(cx,cy,r,c)` | `g_fill_circle` → `host.fill_circle` | 实心圆。 |
| `DrawCircle(cx,cy,r,c)` | `g_draw_circle` → `host.draw_circle` | 描边圆。 |
| `Icon(x,y,sz,bg,letter,lc)` | `g_icon` → `host.icon` | 单字母图标方块（`letter` 为 `int` codepoint，`lc` 字母色）。 |
| `Progress(x,y,w,h,pct,c)` | `g_progress` → `host.progress` | 进度条，`pct` 0–100。 |
| `HasImage(id)` | `g_has_image` → `host.has_image` | 纹理（`.tex`）是否存在；缺失时 C# 回退到扁平主题色。 |
| `Image(id,x,y,w,h)` | `g_image` → `host.image` | 拉伸 blit 纹理。 |
| `Measure(s)` | `g_measure` → `host.measure` | UTF-8 字符串像素宽度。 |
| `Width()` / `Height()` | `g_width` / `g_height` | **客户区**宽高（窗口内）。 |
| `ScreenW()` / `ScreenH()` | `g_screen_w` / `g_screen_h` | **绝对屏幕**尺寸（定位菜单/弹窗用）。 |
| `MouseX()` / `MouseY()` | `g_mouse_x` / `g_mouse_y` | 指针屏幕坐标（原生侧减去客户原点）。 |
| `OriginX()` / `OriginY()` | `g_origin_x` / `g_origin_y` | 当前客户区上下文的窗口原点，控件可据此把局部坐标转屏幕空间。 |
| `SetMouse(x,y)` | `g_set_mouse` | 合成指针位置（语音/自动化点击用，使靠 `MouseX/Y` 命中的控件仍能触发）。 |

> 纹理 id 约定见 `Forms.cs::Tex`：`Wall=0`、`Task=1`、`Menu=2`、`Chrome=3`、`WinBg=4`、`Icon=100+Kind`。资源缺位时 `HasImage()==0`，paint 代码回退扁平色。

---

## 4. `NexOS.Forms.Host` — 机器状态与操作 API

| C# 方法 | 原生实现 | 返回 / 行为 |
|---------|----------|-------------|
| `MemTotalKb()` | `h_mem_total` | 物理内存 KB。 |
| `PagesFree()/PagesUsed()/PagesTotal()` | `h_pages_*` | 页式内存统计。 |
| `HeapAlloc()/HeapFree()` | `h_heap_alloc/free` | CLR 堆已分配/空闲字节。 |
| `HeapAllocCnt()/HeapFreeCnt()` | `h_heap_ac/fc` | 分配/释放计数（诊断）。 |
| `Optimize()` | `h_optimize` | 触发堆整理。 |
| `Hour()/Minute()/Second()` | `h_time_*` | 内核 RTC 时间（`get_time` 回调）。 |
| `OsName()/CpuVendor()/DiskModel()/DiskSizeMb()` | `h_*` | 机器标识字符串 / 磁盘 MB。 |
| `Is64Bit()` | `h_is64` | 1 = 64 位内核。 |
| `PciCount()/NicPresent()` | `h_pci/h_nic` | PCI 设备数 / 网卡是否存在。 |
| `Ticks()/TickMs()` | `h_ticks/h_tick_ms` | 单调毫秒（约 49 天回绕），用于双击检测。 |
| `SetAnim(on)` | `h_set_anim` → `g_mforms_anim` | 请求宿主持续重绘（托管动画如 AI 桌面思考点/打字机效果）；GUI 主循环在置位时把 `render_all()` 节流到 ~30fps。 |
| `RunningMask()` | `h_run_mask` | 第 i 位 = Kind i 窗口已打开，驱动任务栏运行指示。 |
| `FileCount(fs)` | `h_file_count` | 目录条目数（目录项带 `[D]` 前缀视为文件夹）。 |
| `FileName(fs,idx)` | `h_file_name` | 第 idx 条目的文件名（见 §2 空格边界规则）。 |
| `FileIsDir(fs,idx)` | `h_file_is_dir` | 是否目录（`[D]` 前缀）。 |
| `FileRefresh()` | `h_file_refresh` | 使目录列表缓存失效，下次查询重读介质。 |
| `ReadText(fs,name)` | `h_read_text` → `host.read_file` | 读 UTF-8 文本体；NUL/控制字节会被清理（NUL→空格，其它→`.`），防二进制截断。上限 `TEXT_MAX=3072`。 |
| `WriteText(fs,name,text)` | `h_write_text` → `host.write_file` | 持久化文本；返回写入字节数或 -1。上限 `WRITE_MAX=65536`。用于 `nexos.cfg` 与记事本。 |
| `FileMkDir(fs,name)` | `h_file_mkdir` → `host.mkdir` | 建目录，`1`/`-1`。 |
| `FileDelete(fs,name)` | `h_file_delete` → `host.remove` | **删除**（桌面 `fs=3`、文件管理器 `fs=0` 共用）。成功后置 `g_flist_fs=-1` 强制重读。 |
| `FileRename(fs,old,new)` | `h_file_rename` → `host.rename` | 重命名。 |
| `Exec(cmd)` | `h_exec` → `host.exec_command` | 同步执行内核命令，返回输出（上限 `EXEC_MAX=4096`）。 |
| `RunExe(name)` | `h_run_exe` → `host.run_exe` | 经 win32/win64 PE 加载器执行原生 `.exe`，把其窗口呈现到桌面；返回窗口数或负加载错误码。双击 `.exe` 走此路径（而非记事本打开）。 |
| `Shutdown()/Reboot()` | `h_shutdown/h_reboot` | 关机 / 重启。 |
| `Log(s)` | `h_log` | 写 COM1 调试串口（`serial_puts` + `\n`）。 |
| `CharStr(ch)` | `h_charstr` | 把 Unicode codepoint 编码为 UTF-8 托管字符串（CJK 输入经此过 C#↔运行时边界）。 |
| `HttpGet(url)` | `h_http_get` → `host.http_get` | 同步 HTTP GET（Browser 控件用），失败/离线返回 `""`。 |
| `GetClipboard()/SetClipboard(s)` | `h_clip_get/set` | 共享内核剪贴板（与终端 copy/paste 互通）。 |
| `OpenApp(kind)/CloseApp(kind)` | `h_open_app/close_app` → `host.open_app/close_app` | 让内核打开/聚焦/关闭某 Kind 的窗口（托管层自己 Launch 时必需）。 |
| `ExitGui()` | `h_exit_gui` → `host.exit_gui` | 离开 GUI，回到文本终端。 |
| `LoginCheck(user,pass)` | `h_login_check` | 校验凭据；成功则内核提交会话（uid/euid/sudo）并返回 uid，失败返回 -1。 |
| `LoginUid()` | `h_login_uid` | 已登录会话的 uid，-1 表示仍锁屏（从终端重返桌面不再二次要密码）。 |
| `UserCount()/UserName(i)` | `h_user_count/name` | 枚举账户（锁屏用户选择器用）。 |
| `SetPixel(mode,scale,scan)` | `h_set_pixel` → `g_pixel_mode/scale/scan` | 推送复古“像素/CRT”后处理设置给 `gui.cpp::pixelate_framebuffer()`。 |

> **WinForms 宿主**（`csharp/winhost/Backend.cs`）提供与上述**完全一致签名**的真实 GDI+ 实现，`WINHOST` 宏下跳过 `Forms.cs`/`Sys.cs` 的 bodyless internal-call 声明。文件操作在 WinHost 侧映射到 `FsRoot` 下的真实 Windows 目录（`mkfs`/`sfs`/`mkfs/Desktop`）。

---

## 5. `NexOS.Sys` — 基础运行时 API（`NexOS` 命名空间）

| 方法 | 原生绑定（clr.cpp g_icalls） | 说明 |
|------|------------------------------|------|
| `Print(s)` / `PrintInt(v)` / `PrintChar(c)` | `NexOS.Sys::Print*`，输出到内核 | 调试串行输出。 |
| `StrConcat(a,b)` | `NexOS.Sys::StrConcat` | 字符串拼接（优先于手写循环）。 |
| `StrCharAt(s,i)` | — | 取字符。 |
| `StrLen(s)` | — | 长度（无 `string.Length` 依赖时需要）。 |
| `StrSub(s,start,len)` | — | 子串（单次原生分配，避免 O(n²)）。 |
| `StrFlat(s)` | — | 控制符扁平化（单行化）。 |
| `StrEq(a,b)` | — | 相等比较。 |
| `IntToStr(v)` | — | 整数→字符串（无 `int.ToString`）。 |
| `TickCount()` | — | 单调毫秒计数。 |

---

## 6. `MFormsHost` — 内核侧宿主回调表（`mforms.h`）

`gui.cpp` 在 GUI 启动时填充此结构并传给 `mforms_init()`。它是“原生借给托管壳的服务”的契约。所有坐标为**绝对屏幕像素**，裁剪由 mforms 负责，调用方不必裁剪。

**绘图**：`fill_rect / fill_round / draw_round / draw_rect / draw_line / fill_grad / text / text_bg / fill_circle / draw_circle / icon / progress / measure`；纹理 `has_image / image`；`screen_w / screen_h`。

**机器状态**：`mem_total_kb / mem_free_pages / mem_used_pages / mem_total_pages / heap_alloc_bytes / heap_free_bytes / heap_alloc_count / heap_free_count / optimize_memory / tick_ms`。

**文件 / 进程**：`list_files / read_file / write_file / mkdir / remove / rename / http_get / get_time / exec_command / shutdown / reboot / run_exe / exit_gui / open_app / close_app`。

**机器标识**：`os_name / cpu_vendor / disk_model / disk_size_mb / is_64bit / pci_count / nic_present`。

**登录**：`login_check(user,pass)→uid|-1 / login_uid() / user_count() / user_name(idx)`。

> 删除链路即 `Host.FileDelete(fs,name)` → `h_file_delete` → `host.remove` → `gui_cb_remove`（kernel.cpp）→ `mkfs.remove`。桌面与文件管理器两条路径共用 `h_file_name` 取文件名；前期“删不掉快捷方式/文件”的根因就是 `h_file_name` 遇空格截断，已修复（见 §2）。

---

## 7. 生命周期与输入路由（`mforms_*` 原生入口）

`gui.cpp` 通过以下 C 函数驱动托管壳：

| 入口 | 作用 |
|------|------|
| `mforms_init(host)` | 拷贝宿主表 + 注册全部 internal call。须早于 `mforms_start()`。 |
| `mforms_start()` | 加载 `shell.mex`、运行 `Shell::Init`。返回 0 成功；-1 CLR 未就绪，-2 镜像加载失败，-3 Init 故障。`serial` 输出 `[MFORMS] managed shell ready` 即就绪。 |
| `mforms_ready()/mforms_report()` | 就绪态 / 最近失败原因文本。 |
| `mforms_open(kind)/mforms_close(id)` | 按 Kind 实例化窗口 / 关闭；返回 app id≥0。 |
| `mforms_title(id)` | 窗口标题（"" 表示无）。 |
| `mforms_paint(id,ox,oy,w,h)` | 在客户矩形内绘制窗口；每帧回卷堆。 |
| `mforms_click(id,ox,oy,w,h,mx,my)` | 派发点击（屏幕坐标）；非 0 表示已消费。 |
| `mforms_key(id,ch)` | 派发按键（`ch` 为 ASCII 或负虚拟键）。 |
| `mforms_set_mouse(mx,my)` | 每帧刷新指针屏幕坐标（供 hover 态）。 |
| `mforms_paint_desktop(w,h)` | 第 1 层：壁纸 + 桌面图标（窗口之下）。 |
| `mforms_paint_overlay(w,h)` | 第 2 层：任务栏 + 开始菜单（窗口之上）。 |
| `mforms_desktop_click(mx,my)` | 桌面点击 → 返回要启动的 Kind，-1 已消费，-2 不归 shell 管（让 caller 试其窗口）。 |
| `mforms_desktop_rclick / mforms_rclick(id,...)` | 桌面/窗口内右键 → 打开内核原生上下文菜单（文件管理器的文件操作菜单即此路径）。 |
| `mforms_desktop_menu_open()` | 开始菜单是否模态打开。 |
| `mforms_set_running(mask)` | 每帧写入运行窗口位掩码（任务栏指示）。 |
| `mforms_heap_pct()` | 托管堆占用 0–100（任务管理器/诊断用）。 |

**堆纪律（mforms 内部）**：每次 Paint 前 `clr_heap_mark()`，画完 `clr_heap_reset()`；每次事件后 `rebaseline()` 把持久水位抬高到当前；若某事件处理器耗尽堆而 fault，下次调用的首个分配会立即再 fault——`heap_recover()` 把水位回退到上次良好持久点再重试（否则一次坏事件会让壳永久死掉）。持久状态超堆 75% 会打 `[MFORMS] warning`。

---

## 8. 设计要点 / 已知坑

- **右键/文件操作走内核原生菜单**：`mforms_rclick` / `mforms_desktop_rclick` 打开的是内核原生上下文菜单，`DoFileAction`/`DoWinAction`（`App` 虚函数）在菜单项命中后由托管侧回调执行。删除/重命名/新建即经 §4 的 `File*` API。
- **`App` 基类即时模式命中表**：`W.*` 绘图助手在绘制时把每个控件矩形 `RegisterHit` 进实例数组（`hitId/X/Y/W/H`，上限 64）。堆每帧回卷但**实例字段存活**，故右键可命中上次 Paint 注册的控件，不会变“死区”。`OnRightClick` 默认：命中控件则弹“刷新/关闭”菜单。
- **虚拟键码**（`Forms.cs::VK`）：正数为字面 codepoint，负数为合成码（如 `Back=8`、`CtrlC=-3`、`CtrlV=-4`、`CtrlZ=-5`、`CtrlA=-6`、`Up=-18…`、`F1=-40…`、`CtrlPlus=-60`）。`TBox.Key` 据此实现终端风格 Ctrl+C/V/Z/A。
- **主题与持久化**：`Theme` 静态字段（深色壁纸、强调色、任务栏对齐、像素模式等）经 `Host.WriteText(0,"nexos.cfg",…)` 存盘、`ReadText` 加载，重启保留；改动后用 `Host.SetPixel(...)` 推到内核帧缓冲后处理。
- **两套可运行环境**：VM 内核跑真实 internal call（mforms.cpp 桥到 gui.cpp）；Windows `WinHost` 提供同签名 GDI+ 实现，便于在宿主机调试 C# 而无需每次重建镜像。

---

*生成依据：`gui.cpp`（绘图/像素后处理）、`mforms.cpp`（InternalCall 绑定表 `g_regs`、文件操作 `h_*`）、`mforms.h`（`MFormsHost`）、`Forms.cs`（`Gfx`/`Host`/`App`/`W`/`Theme`/`U`）、`Sys.cs`、`Backend.cs`（WinHost 实现）。*
