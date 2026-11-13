# 独立 .mex 应用 — 交互契约与验收文档

> **状态：待评审**。本轮只写文档，未修改代码。
> **配套计划书**：`docs/MEX_STANDALONE_APP_PLAN.md`（现状取证、阻断项、排期）。
> **与需求文档的关系**：`docs/NOTEBOOK_INTERACTION_SPEC.md` v2.0 是**UI / 交互需求基线**（窗口、布局、键位、菜单、保存与关闭逻辑等）。本文件是**实现契约 + 验收判据**，不重复定义 UI；两者冲突时以需求文档为准，并把冲突登记到本文件 §5。
> 取证基线：2026-11-08 工作树；所有「文件:行号」可在评审时核对。

---

## 1. 公共交互契约（所有独立应用必须遵守）

### 1.1 入口签名与调用时机

| 入口 | 当前签名 | 调用时机 | 返回值 / 约束 |
|---|---|---|---|
| `App.Main::Init` | `void Init()`（`Program.cs:40`） | 槽加载成功后，由 `mforms_open_standalone` 手动调用一次（`mforms.cpp:1523`） | 无返回值；**必须在此完成所有静态状态初始化**（MiniCLR 不跑 `.cctor`，`Forms.cs:16-17`） |
| `App.Main::Paint` | `int Paint(int id, int w, int h)`（`:45`） | 每帧，按窗口所属槽分派（`mforms.cpp:1607-1624`） | 返回 0；`w/h` 为客户区尺寸；**禁止在此持久持有新分配对象**（bump 堆每帧回绕，`Forms.cs:10-12`） |
| `App.Main::Click` | `int Click(int id, int x, int y)`（`:52`） | 鼠标点击，按槽分派（`mforms.cpp:1664-1670`） | 返回 0；坐标为窗口客户区相对坐标 |
| `App.Main::Key` | `int Key(int id, int ch)`（`:59`） | 键盘事件，按槽分派（`mforms.cpp:1699-1705`） | **非零 = 已消费**，0 = 继续冒泡；`Tab` 必须消费并 `return` 非零，防止焦点被切走（需求 §4.1） |
| `App.Main::Title` | `string Title(int id)`（`:66`） | 创建窗口时取标题（`gui_legacy.cpp:8612` → `mforms_title`） | 返回字符串；**当前 `mforms_title` 硬编码 `NexOS.Forms.Shell::Title`（`mforms.cpp:1584-1593`），独立应用取不到**，属计划书 B2 |

**`id` 语义**：当前实现把 **槽号当窗口句柄**（`mforms_open_standalone` 返回 `slot`，`mforms.cpp:1530-1540`；`mforms_set_window_slot(slot, slot)`）。因此一槽一窗时 `id` 恒等于槽号；多实例方案确定后（计划书 §10.1）需重新定义 `id → 文档` 映射，并在本表固化。

**冲突登记**：需求 §0 要求「每窗口一次 `Init`」，现状是「每槽加载一次 `Init` 且无参」。见 §5-C1。

### 1.2 启动与关闭时序

```
[桌面双击 / 右键打开 / 文件关联]
  -> launch_app_impl：mkind == -2 分支        gui_legacy.cpp:8596-8604
  -> mforms_standalone_mex / _slot_for 查表   mforms.cpp:1495-1502   [当前编号错配，B1]
  -> clr_load_slot(slot, "X.mex")             mforms.cpp:1511-1519
  -> clr_call("App.Main::Init")               mforms.cpp:1523
  -> mforms_set_window_slot(slot, slot)       mforms.cpp:1533
  -> create_window(APP_MANAGED)               gui_legacy.cpp:8606-8622
  -> mforms_title(id) -> 写入窗口标题         gui_legacy.cpp:8612-8619  [当前未分派，B2]
  -> render_all -> Paint / Click / Key 循环
  -> 关闭窗口 -> mforms_close(id)             mforms.cpp:1575-1582    [当前不释放槽，B3]
  -> clr_close_slot(slot)                     clr.cpp:1477-1490       [当前无调用者]
```

**失败降级**：任一步失败，serial 必须留下可诊断标记（`[MFORMS] standalone load failed` / `App.Main::Init failed` / `[gui] standalone open failed; falling back to the shell`），且桌面不得出现「透明无内容的玻璃窗口」（`gui_legacy.cpp:8630-8640` 的 BUG-9 守卫）。降级目标由计划书 §10.3 决定。

### 1.3 键码约定（现状，需确认是否权威）

独立 Program 目前按**负数控制码**分派（`csharp/apps/mex/Notepad/Program.cs:786-808`）：

| 码 | 含义 | 码 | 含义 |
|---|---|---|---|
| `9` | Tab | `-2` | Enter |
| `-3` | Ctrl+Enter | `-4` | Backspace |
| `-9` | Esc | `-10` | Ctrl+S |

需求 §4 还要求 `Ctrl+A/C/V/X/Z/Y/N/O`、`Ctrl+Shift+S`、`Ctrl+Home/End`、`PageUp/Down`、`Shift+方向键`、`Alt+F4`、`Delete`、`Home/End`、`Up/Down/Left/Right`。**这些组合键在内核里的编码尚未核实**，实现前必须先按 `mforms_key` → GUI 键码映射取一份完整码表并写入本节（待确认项 §5-C4），不得凭猜测编码。

### 1.4 焦点、模态与脏数据

- 同一时刻只有一个活动窗口；`create_window` 后会把其它窗口 `active=false`（`gui_legacy.cpp:8618-8622`）。
- 对话框（Open / Save As / 同名确认 / 关闭拦截）为**模态**：打开时文本区不响应编辑键，`Esc` 关闭当前对话框且不做其它事（需求 §4.4）。
- 任一修改文本的操作（输入、删除、粘贴、撤销/重做）必须置 `isDirty=true`；成功保存后置 `false`（需求 §3）。
- `Alt+F4` / 点关闭按钮时，若 `isDirty=true`，必须走三选项拦截（需求 §7.2）。

### 1.5 绘图约束

- 坐标相对客户区；使用 `Gfx` / `W` / `Btn` / `TBox`（`Forms.cs:33-74, 312-330, 1027-1146`），不直接访问帧缓冲。
- 字符串拼接只能用 `U.Cat` / `U.Sub` / `Sys.StrConcat`（`Corelib.cs:48-51`；`Sys.cs:29-53`），**不能用 `+`**。
- `Paint` 内不得分配后跨帧持有；状态一律放在 `Init()` 里显式初始化的定长数组/字段中（无 `System.Collections`，`Corelib.cs:10-152`）。

### 1.6 禁止事项

1. 引用 `Shell.cs` / `Apps.cs` / `Desktop.cs` 或任何 shell 端类型（独立性的定义，计划书 §2）。
2. 依赖静态字段初始化器、泛型、接口、try/catch、浮点、`string.Format`、`ref/out`（`Forms.cs:9-17`；`McpSdk.cs:23-32`）。
3. 调用 `W.Voice` 或依赖共享窗口右键菜单——`STANDALONE_APP` 下不注册（`Forms.cs:1173-1181, 1360-1366`）。
4. 调用 `Host.DownloadFile*` —— 当前是返回 -1 的未接线 stub（`mforms.cpp:995-1017`）。
5. 在 `Paint` 里做文件 IO 或长循环；文件扫描（如需求 §8 的前 1024 字节探测）应放在打开动作里一次完成。

### 1.7 可用 Host API（白名单摘录）

`ReadText(fs,name)` / `ReadHex(fs,name)` / `WriteText(fs,name,text)` / `FileCount(fs)` / `FileName(fs,idx)` / `FileIsDir(fs,idx)`（`Forms.cs:147-171`；绑定 `mforms.cpp:1215-1231`）；`OpenApp(kind)` / `OpenAppNew(kind)` / `CloseApp(kind)` / `WinAction(id,code)`（`Forms.cs:199-217`；`mforms.cpp:1245-1255`）；`Log(s)`（`Forms.cs:182`）。卷号：`0=MKFS root`、`1=SFS`、`3=Desktop`（需求 §6/§7；`Program.cs` 中 `ReadText(1, path)` 用法见 `Lang.cs:LoadPack`）。

---

## 2. Notepad 具体实现契约（首个参考实现）

> UI 细节以需求文档 v2.0 §1–§9 为准。本节只补充「实现时必须成立」的状态模型、IO 约定与差距清单。

### 2.1 状态模型：按 `id` 隔离

每个窗口持有独立文档对象，至少包含：

```
Doc {
  text, caretRow, caretCol, selStart, selEnd,
  undoStack[<=100], redoStack,          // 需求 §4.1 限制撤销栈深度 100
  isDirty, saved, wrap,                 // wrap 默认 true（需求 §9）
  name, fs,                            // 来源卷，Ctrl+S 原地保存到该卷
  dialog, dialogState                  // none / open / saveas / confirmOverwrite / confirmClose
}
```

**严禁跨窗口共享 `static NotepadApp app`**（需求 §0「多窗口完全隔离」）。MiniCLR 无 Dictionary → 用 `Doc[]` 定长数组 + 按 `id` 线性查找，容量与最大窗口数一致；所有字段在 `Init()` 显式初始化。

### 2.2 布局与命中区（实现坐标）

| 区域 | 需求 | 备注 |
|---|---|---|
| 窗口 | 640×480，最小 320×200 | 需求 §1；**当前内核建窗写死 480×360**（`gui_legacy.cpp:8541-8542`），属差距 D1 |
| 工具栏 | h=36；`[Open] [New] [Save As]` 各 70 px、间距 10 px | 需求 §2 |
| 第二行 | h=20，格式标签 | 需求 §2 |
| 文本区 | y=56 起；左边距 8 px、行高 20 px；垂直滚动条 | 需求 §2；中文按 2 显示宽计算列号（需求 §4.3） |
| 状态栏 | h=22，`Ln X, Col Y` / `字符数 chars` / `Target: 保存路径` | 需求 §2；管道符为显示文本，非表格语法 |
| 标题 | `Notepad - <文件名>` / `Notepad - Untitled`，保存后追加 ` (saved)`，下次编辑清除 | 需求 §1 |

### 2.3 交互判定要点

- **鼠标**：Open / New / Save As / 文本区定位与拖拽选择 / 双击选词 / 滚动条拖动与轨道翻页；状态栏无交互；文本区右键弹出 §5 菜单树且按状态灰化（需求 §3、§5）。
- **键盘**：按需求 §4 全套；`Tab` 插入 4 空格并消费事件；`Ctrl+S` 原地保存到**来源卷**；`Esc` 只关对话框；`Alt+F4` 触发关闭拦截（需求 §4.4、§7.2）。
- **Word Wrap**：默认开；切换后重绘整个文本区并重算滚动范围（需求 §9）。
- **脏标记**：所有编辑路径统一走一个 `MarkDirty()`，避免漏置导致关闭不提示。

### 2.4 文件 IO 约定

| 动作 | API 路径 | 约定 |
|---|---|---|
| 打开 | `Host.FileCount/FileName/FileIsDir` 列目录；`Host.ReadText` 读文本 | 打开时扫描前 1024 字节，含 NUL / 控制字符 / `0x7F` → **拒绝打开并弹窗**，不渲染十六进制视图（需求 §8） |
| 格式识别 | 内容前缀 → 标签与后缀 | 需求 §8 表（`{rtf` / `<?xml` / `<html` / `{` / `;` / `#` / 其它） |
| 浏览卷 | MKFS root(0) / SFS(1) / Desktop(3) | SFS 只读，列表灰化且双击无效（需求 §6） |
| 保存卷 | 仅 MKFS root(0) / Desktop(3) | Save As 只列可写卷（需求 §7.1） |
| 同名冲突 | `Host.WriteText` 前先查存在 | 必须弹「已存在，是否替换」；【否】退回对话框（需求 §7.1；v1 的「直接覆盖」已被 v2.0 改为「确认」） |
| 保存失败 | `WriteText` 返回 < 0 | 红色提示，对话框保持打开（需求 §7.1） |

> `WriteText` 有上限 `WRITE_MAX=65536`（`docs/csharp_managed_api.md:109`）；超出必须提示而不是静默截断。

### 2.5 与当前实现的差距清单

| # | 需求 / 契约 | 现状 | 需改 |
|---|---|---|---|
| D1 | 窗口 640×480 | `gui_legacy.cpp:8541-8542` 写死 480×360 | 建窗尺寸改由应用提供或改默认值 |
| D2 | 标题来自独立 mex | `mforms_title` 硬编码 `Shell::Title`（`mforms.cpp:1584-1593`） | 走 `slot_symbol()`（计划书 B2） |
| D3 | 多窗口数据隔离 | 单个 `static NotepadApp app`，5 个入口忽略 `id`（`Program.cs:38-70`） | `Doc[]` 按 `id` 索引（计划书 B3） |
| D4 | 每窗口一次 Init | 槽加载时一次、无参（`mforms.cpp:1523`） | 明确 `Init(id)` 或定义「每槽一次 + 按需创建 Doc」 |
| D5 | 全套 Ctrl / 方向键 | 仅实现 `-9/-2/-3/-4/-10/9`（`Program.cs:786-808`） | 补齐码表并实现 |
| D6 | 关闭拦截三选项 | 未见实现（当前 `Program.cs` 无 confirmClose 分支） | 新增模态与脏标记判定 |
| D7 | 桌面点击打开独立槽 | 需求 §10 仍标「待接通」；代码 B1 阻断 | 修注册表与回退（计划书 B1） |
| D8 | 任务栏 New window / Close all | 需求 §10 标「无效，待实证」 | 与 B3 一并处理 |
| D9 | 规格称 mex 约 34 KB | 实际 `sfs_files/Notepad.mex` 39,316 B | 文档数字对齐（不改实现） |

---

## 3. 验收用例

| # | 前置 | 操作 | 期望（可观察） |
|---|---|---|---|
| A1 | 镜像含重建后的 `Notepad.mex` | 双击桌面 Notepad | serial 出现 `[MFORMS] standalone app live in slot 1`；窗口标题 `Notepad - Untitled` |
| A2 | A1 | 输入 `abc`，按 Tab，再输入 `d` | 文本为 `abc    d`，焦点仍在文本区（未切走） |
| A3 | A2 | `Ctrl+S`（未命名） | 进入 Save As；选定可写卷并保存成功，标题出现文件名与 ` (saved)`，脏标记清零 |
| A4 | 已有 `notes.txt` | 修改后 `Ctrl+S` | 原地写回**来源卷**，不弹 Save As |
| A5 | 目标卷存在同名文件 | Save As 同名保存 | 弹出替换确认；【否】回到对话框且未写盘，【是】才写入 |
| A6 | 含 NUL 的二进制文件 | 打开 | 弹「无法打开文件：不支持的二进制格式」，不进十六进制视图，不卡死 |
| A7 | 只读卷 SFS | 文件选择器浏览 | 条目灰化，双击无效 |
| A8 | 有未保存修改 | 点关闭 / `Alt+F4` | 出现【保存】【不保存】【取消】；【取消】窗口不关且内容不变 |
| A9 | 文本区右键 | 打开菜单 | 菜单树与需求 §5 一致；撤销栈空时 Undo 灰化、无剪贴板时 Paste 灰化 |
| A10 | 两窗口（多实例方案确定后） | 窗口 A 输入、窗口 B 输入并关闭 B | A 的文本/撤销栈不受影响；B 关闭后槽可回收 |
| A11 | 文本较长 | 切换 Word Wrap | 重绘、滚动范围重算，Home/End 作用于视觉行 |
| A12 | 临时移走 `Notepad.mex` | 双击图标 | serial 有明确失败标记；桌面不出现透明空窗口；按计划书 §10.3 的决策回退到 shell 副本或给出错误提示 |
| A13 | 构建后 | 检查 `sfs_files/Notepad.mex` size/mtime | 与本次 dotnet 构建一致，不是旧 stub（防 Makefile B4 静默保留旧文件） |

---

## 4. 下一个应用需要填的模板

复制本文件结构，只需替换/补齐：

1. **注册表条目**：Kind 值、AppType、`.mex` 名、默认槽（消除手写数字）。
2. **入口契约**：5 个方法签名一致；列出该应用特有的 `id` 状态结构。
3. **布局坐标表**：窗口默认/最小尺寸、各命中矩形。
4. **交互矩阵**：鼠标区域 × 左键/右键、键盘码表、菜单树、模态对话框清单。
5. **IO 约定**：用到的 `Host.*`、卷号、失败与降级。
6. **验收用例**：至少覆盖「能打开、能交互、能关闭、失败可诊断」。
7. **差距清单**：与本文件 §2.5 同样格式登记需求与现状差异。

> 建议第二个应用选 **Calculator**（无文件 IO、无模态、键位少），专门用来验证「模板可复制」与槽分配，不引入新的交互复杂度（计划书 S3）。

---

## 5. 冲突登记与待确认

| # | 冲突 / 待定 | 涉及 | 需要谁决定 |
|---|---|---|---|
| C1 | 「每窗口一次 Init」vs「每槽加载一次 Init」 | 需求 §0 / `mforms.cpp:1523` | 用户确认契约后统一两处文档 |
| C2 | 「多窗口完全隔离」vs「单槽单映像 + 单 static 实例」 | 需求 §0/§1 / `Program.cs:38-70`、`clr.cpp:1413-1414` | 用户选多实例策略（计划书 §10.1） |
| C3 | 需求 §10「桌面点击走 Shell.Open」vs 源码已有 `-2` standalone 分支 | 需求文档 / `gui_legacy.cpp:8596-8604` | 实测当前镜像后更新需求文档（本轮不改它） |
| C4 | Ctrl / 方向键 / Alt+F4 的完整键码表未在文档中定义 | 需求 §4 / `Program.cs:786-808` / 内核键码映射 | 实现前补一份权威码表并回填 §1.3 |
| C5 | 窗口尺寸 640×480 vs 内核建窗 480×360 | 需求 §1 / `gui_legacy.cpp:8541-8542` | 确认由谁提供尺寸 |
| C6 | 规格称 34 KB vs 实际 39,316 B | 需求 §0 / `sfs_files/Notepad.mex` | 仅文档数字对齐 |
| C7 | 需求文档正在被并行改写（17:03 v2.0） | 全文件 | 冻结基线后才进入实现阶段 |

> 以上 C1–C7 未确认前，不开始写实现代码。
