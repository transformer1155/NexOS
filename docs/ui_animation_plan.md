# NexOS UI 控件动画化规划书

> 目标：让 NexOS 里**每一个 UI 控件都有动画**——hover 过渡、按压回弹、状态切换、入场/退场，
> 对齐 WinUI 3 的 Fluent 动效语言（300ms 缓动、press 收缩、hover 淡入）。
>
> 本文档建立在已完成的渲染优化之上（BIOS 默认字体、脏矩形合成、桌面层脏区跳过），
> 并依赖 `NexOS.Forms`（`csharp/NexOS.Forms/Forms.cs`）现有的动画底座。

---

## 1. 现状与底座

### 已有（可直接复用）
| 机制 | 位置 | 说明 |
|------|------|------|
| 时间源 | `Host.TickMs()`（`Forms.cs:96`，`[InternalCall]`） | 毫秒级时钟，VM/宿主导一致 |
| 重绘驱动 | `Host.RequestRepaint()`（注释见 `Forms.cs:97`） | 动画期间持续请求重绘 |
| 按压收缩动画 | `Btn` 类（`Forms.cs:178-213`） | `ScaleAt`：press→50% 收缩(100ms)→保持(500ms)→弹回(150ms) |
| Hover 重着色 | `W.Hot(x,y,w,h)`（`Forms.cs:708`） | 命中检测 + 颜色切换（**但瞬时，无过渡**） |
| 窗口开关动画 | 内核 `Win11Window.anim_state` | 窗口 opening/pressing 动画（已实现） |

### 缺口（本规划要补）
1. **无通用动画框架**：`Btn` 是手写特例，没有可复用的 `Anim`（插值/easing/状态机）。
2. **hover 是瞬变**：`Hot()` 返回 bool，颜色立即跳变，无 150ms 淡入淡出。
3. **状态控件无动画**：`ToggleSwitch`/`CheckBox`/`RadioButton`/`Slider`/`ProgressBar` 未定义（各 app 手绘），且无切换过渡。
4. **入场/退场无动画**：`Card`/`Panel`/`Row`/弹窗出现是硬切。
5. **焦点环无动画**：`TextBox` 聚焦无 Fluent 焦点环过渡。

结论：动画化 = **建一个通用 `Anim` 框架** + **把 hover/press/状态切换/入场 接到每类控件**。

---

## 2. 通用动画框架（新增 `NexOS.Forms/Anim.cs`）

所有控件动画走同一套时间状态机，避免 `Btn` 式重复代码。

### 2.1 基础工具
```csharp
static class Ease {
    static double Linear(double t) => t;
    static double Quad(double t) => t < 0.5 ? 2*t*t : 1 - (-2*t+2).Pow2()/2;
    static double Cubic(double t)  => /* 标准 ease-in-out */
    static double Back(double t)   => /* press 回弹过冲 */
    static double Lerp(int a, int b, double t) => (int)(a + (b-a)*t);
    static uint  LerpColor(uint c0, uint c1, double t);   // 逐通道插值
}
```

### 2.2 单值动画状态机 `Tween`
```csharp
class Tween {
    double from, to, durMs; int startMs;
    double Value(int now) => Ease.Cubic((double)(now-startMs)/durMs);  // 0..1
    bool Done(int now) => now - startMs >= durMs;
    void Restart(double to, int now);   // 支持反向（hover 离开时回 0）
}
```
- 每个**可交互控件**持有一个 `Tween hover`（0=未悬停,1=悬停）和 `Tween press`（0..1）。
- 每帧 `App.Paint` 用 `TickMs()` 推进，值用于插值颜色/缩放。

### 2.3 动画驱动循环（关键）
- 现有 `Host` 在动画进行中持续 `RequestRepaint()`（见 `Forms.cs:97` 注释）。
- 引入 `Anim.IsActive()`：只要有任何 `Tween` 未完成，就保持重绘请求；全部静止时停止（配合桌面层脏区跳过，**空闲零 CPU**）。
- 与内核脏矩形协同：控件 `Invalidate()` 标记自身 bbox → 内核 `present_rect` 只 flip 该区（见 `WinUI_port_plan.md` 第 4 节）。

---

## 3. 逐控件动画清单（"每个 UI 都做动画"）

| 控件 | 现有 | 动画化方案 | 参数（对齐 Fluent） |
|------|------|-----------|--------------------|
| **Button / Primary** | `Btn` 收缩 | 保留 press 收缩；**新增 hover 淡入**（fill 从 `Card`→`AccentHi` 150ms ease） | press 100/500/150ms；hover 150ms |
| **Key**（计算器） | `Btn` 收缩 | 同 Button；accent 键 hover 加亮过渡 | 同上 |
| **ToggleSwitch** | 无（新建） | 旋钮 `translateX` 滑动 200ms `Back` 过冲；底色 `Accent`↔灰 过渡 | on/off 200ms |
| **CheckBox** | 无（新建） | 勾选时打勾 `scale 0→1` + 轻微 `Back` 回弹；边框 hover 淡入 | check 120ms |
| **RadioButton** | 无（新建） | 选中时内点 `scale 0→1` 弹入；同组切换时旧点缩出 | 120ms |
| **Slider** | 无（新建） | 拇指 hover 放大 1.0→1.15；拖动时跟手（无缓动，避免拖滞）；释放回弹 | hover 100ms |
| **ProgressBar** | `Meter` 静态 | 值变化时**宽度补间**（从旧 pct 滑到新 pct 300ms ease），非硬跳 | 300ms |
| **ComboBox / Dropdown** | 无（新建） | 展开时选项面板上滑 + 淡入（y 偏移 8px→0, alpha 0→1, 150ms） | 150ms |
| **TextBox** | 无（新建） | 聚焦时 **Fluent 焦点环**（`BorderMid`→`Accent` 描边 + 1px 外扩 150ms）；光标 blink 已有 | focus 150ms |
| **Card / Panel** | 静态 | 入场 `scale 0.98→1` + alpha 0→1（100ms，仅首次绘制/列表增量项） | 100ms |
| **Row**（列表项） | hover 瞬变 | hover 淡入 `Hover` 底；selected 选中条 `width 0→3` 滑入 | hover 120ms |
| **Header / Label / Sub** | 静态 | 可选：文本出现淡入（设置页切换时） | 80ms |
| **ContentDialog / Popup** | 硬切 | 入场**中心放大 0.9→1 + 遮罩 alpha 0→0.4**（150ms）；退场反向 | 150ms |
| **Start Menu / Taskbar 项** | 硬切 | 打开：菜单从任务栏上滑 + 淡入；项 stagger 逐个 30ms 延迟入场 | 150ms + stagger |
| **Tab / Pivot** | 无 | 切换时内容淡入 + 指示条 `translateX` 滑动 | 200ms |
| **Toast / 通知** | 无 | 从右下角滑入 + 淡入；自动退场滑出 | 200ms |
| **ScrollViewer** | 无 | 滚动惯性 + 滚动条 thumb 淡入淡出（hover 显示） | 150ms |
| **Tooltip** | 无 | 延迟 500ms 后出现 + 淡入上浮 4px | 100ms |

**统一原则**：
- 所有 hover/press 颜色与缩放**必须插值**，绝不 `Hot()` 硬跳。
- 所有"状态切换"（on/off、selected、focus、open）**必须补间**，不硬切。
- 入场/退场**必须有缩放+透明度过渡**（Fluent 标志）。
- 时长集中在 `Anim.Const`（hover 150 / press 100/500/150 / state 200 / dialog 150），便于统一调。

---

## 4. 性能与渲染优化协同

动画是"持续重绘"的来源，必须配合已落地的渲染优化，否则抵消收益：

1. **桌面层脏区跳过**（已落地）：动画只发生在某个窗口内 → 窗口 sig 不变（窗口几何未变）→ 桌面层跳过（墙纸+图标不重绘）。✅
2. **控件级脏区**（待 `WinUI_port_plan.md` 4.2）：控件 `Invalidate()` 标自身 bbox → 内核 `present_rect` 只 flip 该控件区，**动画帧不整屏 present**。⚠️ 需先做。
3. **空闲停绘**：`Anim.IsActive()` 为假（所有 Tween 完成）时停止 `RequestRepaint` → 静态界面 CPU≈0（配合主循环空闲不调 `render_all`）。✅
4. **BIOS 字体**（已落地）：动画文字用 1-bit 位图，重绘便宜。
5. **省内存**（512 MB VM）：动画状态机每控件仅 ~2 个 `Tween`（~32 字节），12 app 总开销可忽略。

> 顺序建议：**先完成控件级脏区（`WinUI_port_plan.md` 4.2）**，再做动画——否则动画期间每帧全屏 present 会抵消桌面跳过收益。

---

## 5. 分阶段交付

### 阶段 A — 动画框架（1 周）
- `NexOS.Forms/Anim.cs`：`Ease` / `Tween` / `Anim.IsActive()` / `LerpColor`。
- 把 `Btn` 的 `ScaleAt` 重写为基于 `Tween`（行为不变，验证了框架）。
- `W.Hot` 升级为 `W.HoverState(id,x,y,w,h)` 返回 0..1 插值（保留 `Hot()` 兼容）。

### 阶段 B — 基础控件动画（1–2 周）
- Button/Primary/Key：hover 淡入（接 `HoverState`）。
- Row：hover/selected 过渡。
- Card/Panel/Header：入场淡入缩放。
- TextBox（新建）+ 焦点环动画。

### 阶段 C — 状态控件动画（2 周，含控件新建）
- ToggleSwitch / CheckBox / RadioButton：切换补间 + 回弹。
- Slider / ProgressBar：值补间。
- ComboBox / Dropdown：展开上滑淡入。

### 阶段 D — 容器/浮层动画（1–2 周）
- ContentDialog / Popup：缩放+遮罩淡入退场。
- Start Menu / Taskbar 项 / Toast / Tooltip / Tab / ScrollViewer。

### 阶段 E — 接入真实 app + 性能收口（持续）
- 把 12 个 app 的"手绘控件"迁移到 `NexOS.Forms` 动画控件。
- 完成后台：控件级脏区 + `Anim.IsActive` 空闲停绘。
- 在 512 MB VM 验证：动画流畅、静态零 CPU。

---

## 6. 验收标准

- [ ] `Anim.cs` 存在；`Btn` 行为不变（回归通过）。
- [ ] 列表中**每一类控件**（§3 表格）都有至少一种动画（hover/press/状态/入场其一）。
- [ ] 所有颜色/缩放切换均为插值，**无 `Hot()` 硬跳**（代码审查）。
- [ ] 动画期间只 present 脏区（控件 bbox），非全屏。
- [ ] 所有动画静止后 `RequestRepaint` 停止，静态桌面 CPU≈0（与桌面层跳过协同）。
- [ ] 12 app 在 512 MB VM 内动画流畅（≥30fps 过渡）。

---

## 7. 与既有文档关系

- `WinUI_port_plan.md`：控件动画依赖其 §4.2 控件级脏区（否则动画帧全屏 present）。
- 渲染优化（BIOS 字体 + 脏矩形 + 桌面层跳过）：本规划的**性能前提**，已落地。
- 时间驱动统一使用 `Host.TickMs()`，保证 VM 与 WinForms 宿主动效一致（已在 `Btn` 验证）。
