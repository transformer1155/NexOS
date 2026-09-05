# NexOS × WinUI 3 搬运计划书

> 目标：在 NexOS 自有的即时模式 GUI（`NexOS.Forms`，见 `csharp/NexOS.Forms/Forms.cs`）之上，
> 构建一套 **WinUI 3 风格**的 UI 框架子集，使应用能用接近 WinUI 的控件 / 布局 / 主题 / XAML 写法开发，
> 并把 `win11-ui/`（WinUIonWeb 参考实现 + `winui3-tokens.json` 设计令牌 + `SEGOEICONS.TTF` 图标字体）
> 作为设计与资源来源"搬运"进来。
>
> 本文档与本次已落地的两项渲染优化（**BIOS 默认字体** + **脏矩形合成框架**）配合使用。

---

## 0. 现状与约束（为什么不能直接搬 WinUI 二进制）

| 维度 | Windows / WinUI 3 | NexOS 现状 |
|------|-------------------|-----------|
| 渲染模型 | 保留模式（Visual Tree + Composition） | 即时模式（`Gfx.*` 静态绘制，`App.Paint` 回调） |
| 依赖 | WinRT / DWM / DirectComposition | 自有内核合成器（`gui.cpp` 软合成 + LFB） |
| 控件 | `Microsoft.UI.Xaml.Controls.*` | 无控件树，只有 `Fill/Text/Icon/Image` 原语 |
| 布局 | `Grid / StackPanel / RelativePanel` + Measure/Arrange | 手工 `x,y,w,h` 坐标 |
| 主题 | `ResourceDictionary` + Fluent 设计令牌 | 硬编码 `C.*` 颜色常量 |
| 字体 | Segoe UI（矢量） | 现默认 **BIOS 8×16 1-bit**（本次改动，`g_font_mode=0`） |

结论：**无法搬运 WinUI 的二进制或 WinRT 运行时**。可行的是"语义搬运"——
对齐 WinUI 3 的 **API 形状、布局语义、Fluent 视觉令牌、控件行为**，渲染落在 NexOS 自有合成器上。
这是"搬运设计 + 重建实现"，而非"搬运实现"。

---

## 1. 总体架构

```
应用代码 (C#)
   │  使用 WinUI 风格 API：new Button(){ Content="OK", Click+=... }
   ▼
NexOS.WinUI  (新增，保留模式层)
   ├─ Visual / UIElement 基类（Measure / Arrange / Render 三阶段）
   ├─ Layout 面板：Grid / StackPanel / RelativePanel / Canvas
   ├─ 核心控件：Button / TextBox / CheckBox / RadioButton / ComboBox /
   │            ListView / Slider / ProgressBar / ToggleSwitch / ContentDialog
   ├─ 资源系统：ResourceDictionary + ThemeResource + Fluent 令牌
   ├─ XAML 加载器：把 .xaml 解析为 Visual Tree（可选，分阶段）
   └─ 命中测试 / 路由事件 / 焦点 / 键盘导航
   ▼  (每帧) 调 NexOS.Forms 原语  →  gui.cpp 合成器（脏矩形 present_rect）
```

- **不破坏现有即时模式**：`NexOS.WinUI` 是 `NexOS.Forms` 之上的一层；现有 app 仍可继续手写坐标。
- **渲染复用**：WinUI 控件的 `Render` 最终调用 `Gfx.FillRound/Text/Image/Icon`，与现在 shell 绘制一致。
- **主题令牌来源**：直接吃 `win11-ui/winui3-tokens.json`（色板、间距、圆角、字号、阴影）与 `SEGOEICONS.TTF`（图标）。

---

## 2. 分阶段交付

### 阶段 A — 布局与基础 Visual（1–2 周）
- `UIElement`：`Width/Height/Margin/Padding/HorizontalAlignment/VerticalAlignment`、`Visibility`。
- 三阶段管线：`Measure(available)` → `Arrange(rect)` → `Render(ctx)`，对齐 WinUI 语义。
- 面板：`StackPanel`（H/V）、`Grid`（行列 `*`/`Auto`/`Npx`）、`Canvas`、`RelativePanel`（最小子集）。
- 使现有 ControlPanel / Settings 页能用 `Grid` 重写（验证布局引擎）。

### 阶段 B — 核心控件（2–3 周）
- 基础：`Button`、`TextBox`（复用现有 `App` 文本输入）、`TextBlock`、`Image`、`Border`、`Rectangle`。
- 选择类：`CheckBox`、`RadioButton`、`ToggleSwitch`、`ComboBox`。
- 列表类：`ListView` / `ItemsRepeater`（虚拟化可选，先做非虚拟化）。
- 反馈类：`ProgressBar`、`Slider`、`ToolTip`、`ContentDialog`（复用 shell 模态框机制）。
- 每个控件 = 一个 `Visual` 子类，命中测试 + 路由事件（`Click`、`TextChanged`、`SelectionChanged`）。

### 阶段 C — 主题与 Fluent 资源（1 周）
- `ResourceDictionary` + `ThemeResource`（`Light`/`Dark` 自动切换）。
- 从 `winui3-tokens.json` 生成 `Fluent.Tokens.*` 静态表（Accent、Layer、Stroke、CornerRadius、Spacing）。
- 把现有 `C.*` 硬编码颜色逐步替换为令牌引用，统一视觉。
- 搬运 `SEGOEICONS.TTF` → 作为图标字体源，提供 `SegoeIcon("Save")` 之类的映射（替代当前位图图标 `Tex.Icon`）。

### 阶段 D — XAML 加载器（2–3 周，可延后）
- 最小 XAML 子集解析：`<Page>`/`<Grid>`/`<Button Content=".." Click=".."/>`。
- 与设计令牌联动（`{ThemeResource AccentFillColor}`）。
- 目标：让 UI 描述与逻辑分离，降低 app 体积（XAML 比手写 C# 布局更短）。
- 若工期紧，可先做 **C# 流式构建 API**（`new Grid(){...}`）而暂缓 XAML 解析器。

### 阶段 E — 应用迁移 + 性能收口（持续）
- 把 12 个内置 app（Calculator / Notepad / Browser / Terminal / ControlPanel / 等）逐步迁到 WinUI 风格控件。
- 复用本次已落地的：
  - **脏矩形合成**（`gui.cpp` 的 `g_dirty` + `present_rect`）：控件局部重绘时只 flip 脏区。
  - **BIOS 默认字体**（`g_font_mode=0`）：保持文字细、省内存；需要 Segoe 矢量美观时切 `g_font_mode=1`。
- 把"全屏重绘 backbuffer"逐步收敛为"局部脏区重绘"（见第 4 节）。

---

## 3. 资源搬运清单（来自 `win11-ui/`）

| 资源 | 路径 | 用途 |
|------|------|------|
| Fluent 设计令牌 | `win11-ui/winui3-tokens.json` | 生成 `Fluent.Tokens` 静态表 |
| Segoe 图标字体 | `win11-ui/SEGOEICONS.TTF` | 矢量图标（替代位图 `Tex.Icon`） |
| WinUIonWeb 参考 | `win11-ui/WinUIonWeb/` | 控件视觉/交互参考（非直接编译） |
| WinUI 文档/类型签名 | `win11-ui/WinUIonWeb/.../*.md` | API 形状对齐依据 |

**不搬运**：WinUIonWeb 的 JS/TS 运行时、DirectComposition 后端、WinRT projection。

---

## 4. 与本次渲染优化的衔接（必须项）

### 4.1 已落地的内核侧局部重绘（`gui.cpp`，已实现）
- **桌面层脏区跳过**：`render_all()` 用 `desk_sig()` 对可见窗口集（id + 几何 + 最小化/激活态）做哈希。仅当哈希变化或 `g_desk_needs_full`（首帧 / pixelate 切换）时，才调用 `mforms_paint_desktop()` 全屏重绘墙纸+图标+任务栏；**静态桌面下完全跳过该调用**，backbuffer 桌面层保留。
  - 窗口开/关、最小化/还原、焦点切换、拖动、缩放——任一变化都会被 `desk_sig()` 自动捕获（无需改各调用点）。
  - pixelate（CRT）模式切换经 `Host.SetPixel` 推送，检测 `g_pixel_mode` 变化触发全屏重绘。
- **局部组件脏区**：popup / 文件管理器右键菜单 / 属性框 / IME 候选条 / 任务栏条，各自 `dirty_add` 自身 bbox，`present_rect` 只 flip 该区域（不再全屏 present）。
- **空闲零开销**：主循环静态无事件时不调 `render_all`（鼠标移动走独立 `present_rect` 路径），CPU≈0。

### 4.2 待 C# 侧补强（下一阶段）
当前墙纸+图标全屏重绘已避开，但 **任务栏（时钟每秒变）与窗口层每帧仍重绘**（窗口层重绘属内核，成本有限）：
1. **C# 侧 `Desktop.PaintDesktop(dirtyRect)` 局部化**：墙纸缓存为一次性 bitmap，只在脏区 blit + 重绘脏区内窗口/图标；任务栏拆分为独立脏区（时钟单独），避免每秒全屏 present。
2. **控件级脏区**：`UIElement.Invalidate()` 标记自身 bbox → 合并进 `g_dirty` → `present_rect` 只 flip 该区域，进一步降低窗口层重绘成本。

预期效果（512 MB VM）：静态桌面几乎零 CPU 合成开销；仅打字/拖拽窗口时局部重绘。

---

## 5. 风险与对策

| 风险 | 对策 |
|------|------|
| 保留模式 + 即时模式双栈内存占用上升（512 MB 紧张） | 控件树按需创建/销毁；共享 backbuffer；BIOS 字体模式省 RAM |
| XAML 解析器工期不可控 | 阶段 D 可降级为 C# 流式构建 API |
| Fluent 令牌与现有 `C.*` 冲突 | 阶段 C 做兼容层，`C.*` 改为令牌别名 |
| 现有 12 app 迁移成本 | 阶段 E 增量迁移，新旧 API 并存 |
| Segoe 矢量字体渲染慢 | 默认 BIOS 位图字体，Segoe 仅用于图标/标题，或 `g_font_mode=1` 按需 |

---

## 6. 验收标准

- [ ] `NexOS.WinUI` 可在 `NexOS.Forms` 之上运行，编译进 `shell.mex`。
- [ ] 至少 ControlPanel + Settings 用 `Grid`/`StackPanel` + 原生 `Button`/`TextBox`/`ToggleSwitch` 重写。
- [ ] `Fluent.Tokens` 来自 `winui3-tokens.json`，Dark/Light 切换生效。
- [ ] `SegoeIcon` 可用，替代部分 `Tex.Icon` 位图。
- [ ] 静态桌面下 `render_all` 触发率≈0（仅交互局部重绘，依托脏矩形）。
- [ ] 12 个 app 在 512 MB VM 内启动/操作流畅（结合本计划书与已落地优化）。

---

## 7. 本次已落地（前置成果）

1. **BIOS 默认字体**：`gui.cpp` 新增 `g_font_mode`（默认 0）。
   - `draw_char` / `draw_cjk` / `draw_cjk_transparent` 在 `g_font_mode==0` 时跳过矢量与 AA 拉丁路径，强制走 1-bit 8×16 BIOS 位图（细、无 AA）。
   - `gui_init` 在 BIOS 模式下跳过 `load_font_la` / `load_font_la16` / `vec_init`，省内存。
   - 需要 Segoe 矢量美观时，调用方在 `gui_init()` 前设 `g_font_mode = 1`。
2. **脏矩形合成框架**：`gui.cpp` 新增 `g_dirty` 并集 + `dirty_reset/dirty_add`，
   `render_all` 末尾改走 `present_rect(g_dirty)`（桌面全屏重绘时退化为全屏 flip，正确性不变），
   光标区域单独 `dirty_add`。主循环鼠标移动路径已用 `present_rect` 局部 flip。
