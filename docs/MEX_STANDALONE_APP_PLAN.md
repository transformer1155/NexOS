# 独立于 Shell 的 .mex 应用 — 实施计划书

> **状态：待评审**。本轮只产出文档，未修改任何代码、未改动已有规格文档。
> **取证基线**：2026-11-08 工作树。需求基线为 `docs/NOTEBOOK_INTERACTION_SPEC.md` v2.0（该文件 17:03 仍在被并行改写，本计划书以读到的 v2.0 为准）。
> **配套文档**：`docs/MEX_APP_INTERACTION_SPEC.md`（实现契约、交互细则、验收用例）。
> 文中所有「文件:行号」均指向当前工作树，用于评审时核对，不是修改建议的既成事实。

---

## 1. 结论先行

框架已经搭好，但**独立应用的启动链路当前是断的**；现在不是「开始写第 2 个应用」的时机，而是先把 Notepad 这唯一一条链路修通、实测、固化成模板。

| 维度 | 现状 | 证据 |
|---|---|---|
| 打包 | 可用。`MEX1` 离线展平格式，入口按 `Type::Method` 字符串解析 | `tools/mex_pack.py:689-696, 718-739`；`clr.cpp:1189-1229, 1553-1573` |
| 槽 | 可用。共 4 槽（`CLR_MAX_VMS=4`），槽 0 = shell，应用从 1 起 | `clr.cpp:225-230`；`clr.h:93-101`；`mforms.cpp:1490-1493` |
| 事件分派 | 部分可用。Paint / Click / Key 按窗口所属槽分派 | `mforms.cpp:1458-1472, 1607-1624, 1664-1670, 1699-1705` |
| 启动分支 | **断**。`APP_NOTEPAD = 11`，而查找函数判断 `app == 7` | `src/gui/gui_legacy.cpp:1902-1923` vs `mforms.cpp:1495, 1500` |
| 失败回退 | **断**。standalone 失败后 `mkind = -1`，连 shell 内置路径也绕过 | `gui_legacy.cpp:8627-8630, 8697` |
| 应用产物 | 仅 1 个真实 mex：`sfs_files/Notepad.mex`（39,316 B）；其余 12 个是 115 KB 无构建规则 stub | `sfs_files/*.mex`；`Makefile:751-753` |
| 默认构建 | `csharp:` 只含 `hello.mex` + `shell.mex`，`SFS_IMG` 显式依赖也只列这两个 | `Makefile:812`、`Makefile:649` |
| 桌面可见性 | 只放行 Notepad（图标 / 任务栏 / Start） | `Desktop.cs:551-555, 725-726, 2830-2833, 918-934` |

> 注：`$(SFS_IMG)` 的 `$(wildcard $(SFS_DIR)/*)` 会顺带把 `Notepad.mex` 扫进 SFS 镜像，所以**镜像里有它，但默认构建不会重新生成它**（`Makefile:649`）。

---

## 2. 目标与范围

| 项 | 约定 |
|---|---|
| 「独立应用」定义 | 一个 `.mex`，只链接 `NexOS.Forms` / `NexOS.Core`，**不引用** `Apps.cs` / `Desktop.cs` / `Shell.cs`；编译期宏 `STANDALONE_APP` 去掉语音与共享窗口菜单绑定（`csharp/apps/mex/Notepad/Notepad.csproj:45-64`；`Forms.cs:1173-1181, 1360-1366`） |
| 入口契约 | `App.Main::Init / Paint / Click / Key / Title`（`csharp/apps/mex/Notepad/Program.cs:36-71`） |
| 运行形态 | 非 0 CLR 槽；窗口通过 `mforms_set_window_slot(mid, slot)` 绑定（`gui_legacy.cpp:8613`） |
| 首个落地对象 | Notepad（`csharp/apps/mex/Notepad/`） |
| 后续对象 | `csharp/apps/mex/` 下另 11 个目录；Kind 表见 `Shell.cs:15-41` |
| 本轮不做 | 不写实现代码、不改内核调度、不修改 `NOTEBOOK_INTERACTION_SPEC.md`、不动 `sfs_files/*.mex` |

---

## 3. P0 阻断项（未修通前，写多少应用都点不起来）

| # | 问题 | 证据 | 建议修法（待评审，未实施） |
|---|---|---|---|
| **B1** | 编号错配 + 回退错误。`launch_app_impl` 传入 AppType（Notepad=11），`mforms_standalone_mex/slot_for` 只认 `app==7`（7 实际是 `APP_MEM_OPTIMIZER`）→ 返回 NULL / -1 → `mforms_open_standalone` 直接失败；随后 `mkind=-1` 使 `mforms_open()` 分支不成立 | `gui_legacy.cpp:8596-8604, 8627-8630, 8697, 1902-1923`；`mforms.cpp:1493-1507` | 建立单一注册表（见 §4），按 `APP_NOTEPAD` 常量查表；失败回退改为 `mkind = managed_kind_for(app)`，让 shell 内置副本兜底 |
| **B2** | `slot_symbol()` 只覆盖 Paint / Click / Key；`mforms_title` / `mforms_close` / `mforms_paint_pan` / `mforms_drag` 仍硬编码 `NexOS.Forms.Shell::*`，独立窗口取不到自己的标题、关闭回调进 shell | `mforms.cpp:1454-1457, 1584-1593, 1575-1582, 1642-1651, 1676-1694` | 四者统一走 `slot_symbol()`；Title 需保留 `clr_heap_mark/reset` 语义 |
| **B3** | 多实例不支持。槽号固定返回 1；`clr_load_slot` 复用槽会替换并重置映像；`clr_close_slot` 全仓库无调用者；独立 Program 只有一个 `static NotepadApp app` 且 5 个入口全部忽略 `id` | `mforms.cpp:1499-1502`；`clr.cpp:1413-1414, 1477-1490`；`Program.cs:38-70` | 引入槽分配/释放 + 按 `id` 索引的定长文档表（MiniCLR 无 Dictionary）；关闭窗口时调 `clr_close_slot` |
| **B4** | 默认构建不重建独立 mex；dotnet SDK 缺失时规则只警告并保留旧二进制 | `Makefile:760-761, 812, 649` | 把 `<App>.mex` 加入 `csharp:` 与 `$(SFS_IMG)` 显式依赖；构建后校验 size/mtime |
| **B5** | 桌面/任务栏/Start 只放行 Notepad，其余 17 个 Kind 被强制隐藏 | `Desktop.cs:2830-2833, 918-934, 551-555` | 改为「已从 SFS 找到对应 mex 才放行」，新应用上线后自动出现 |
| **B6** | 显示名数组 `kName[6] = "Notepad"` 与 `Kind.Notepad = 7` 错位 | `Desktop.cs:632-637` vs `Shell.cs:24` | 由注册表生成显示名，删除手写下标 |
| **B7** | 入口契约未成文：`Init()` 当前无参、只在槽加载时调一次，与 v2.0 规格「每窗口一次 Init」不符；`Key` 返回值语义未统一 | `Program.cs:40-70`；`mforms.cpp:1523, 1699-1705`；`NOTEBOOK_INTERACTION_SPEC.md:20` | 在交互文档中固化签名与时序（见配套文档 §1），再按契约改实现 |

---

## 4. 目标架构：一张注册表 + 一套契约

1. **应用注册表（单一真源）**：把 `Shell.cs` 的 Kind 值、`gui_legacy.cpp` 的 AppType、`.mex` 文件名、默认槽号合并成一张表（建议新头文件或 `mforms.cpp` 内常量表）。目标是消灭目前三套编号：`Kind.Notepad=7`、`APP_NOTEPAD=11`、硬编码 `app==7`。
2. **入口契约**：`App.Main::Init / Paint(int id,int w,int h) / Click(int id,int x,int y) / Key(int id,int ch) / Title(int id)`；`id` 必须用于区分窗口，`Key` 返回非零表示已消费。详见配套交互文档 §1.1。
3. **槽生命周期**：4 槽（shell + 最多 3 个应用）。分配 → 加载 mex → Init → 建窗并绑定槽 → 事件分派 → 关闭窗口 → 释放槽。当前 `mforms_open_standalone` 直接把 **slot 号当窗口句柄返回**（`mforms.cpp:1530-1540`），这会限制一槽一窗；若采纳多实例，需改为独立 handle + 槽引用计数。
4. **每窗口状态**：MiniCLR 无 `System.Collections`、无静态初始化器、bump 堆每帧回绕（`Forms.cs:9-17`；`Corelib.cs:10-152`）→ 用定长数组 + 线性查找，所有初始化放 `Init()`，禁止在 `Paint()` 里持久持有新分配对象。
5. **API 白名单**：`Host.ReadText / ReadHex / WriteText / FileCount / FileName / FileIsDir / OpenApp / Log / RunExe` 等可用（`Forms.cs:147-217`；绑定 `mforms.cpp:1184-1278`）。禁止：`string` 用 `+`、静态字段初始化器、引用 Shell/Desktop、`W.Voice`、以及尚未接通的 `DownloadFile*` stub（`mforms.cpp:995-1017`）。

---

## 5. 构建与部署流程

标准环境（PowerShell → `vm-run`，见项目记忆；`vm-run` 参数里禁止 `| ; &&`，复杂逻辑先 push 脚本）：

```
vm-run status                              # 工具链 / SSH / 共享目录自检
vm-run "bash tools/verify/preflight_idle.sh"   # 确认没有别的 make / qemu 在跑
vm-run make                                # 构建（自带 flock）
vm-run test-log                            # 无头启动 120s，回读串口
vm-run push verify_x.sh /tmp/verify_x.sh    # 复杂验证脚本
vm-run "bash /tmp/verify_x.sh"
```

Makefile 侧需要补齐的形态（**示意，未修改**）：

```
csharp: $(SFS_DIR)/hello.mex $(SFS_DIR)/shell.mex $(SFS_DIR)/Notepad.mex $(SFS_DIR)/<App>.mex
$(SFS_IMG): ... $(SFS_DIR)/Notepad.mex $(SFS_DIR)/<App>.mex
$(SFS_DIR)/<App>.mex: csharp/apps/mex/<App>/Program.cs ... $(CS_CORE) tools/mex_pack.py
    dotnet build ... -c Release && python3 tools/mex_pack.py <dll> $@ "App.Main::Init"
```

注意 `vm-run status` 实测无 `/dev/kvm`、`svm-flags=0` → **QEMU 一律 `-accel tcg`**；CPU 重活必须走 `tools/verify/with_build_lock.sh`（与 vm-run make/test 共用 `/tmp/nexos_vm.lock`）。

---

## 6. 启动事件链（供实现与验收对照）

```
双击桌面图标                     gui_legacy.cpp:7517-7540
  -> mforms_desktop_click        mforms.cpp:1855-1865  -> Shell::DesktopClick
  -> Desktop.Click 返回 Kind     Shell.cs:413-416 / Desktop.cs:14-17
  -> app_for_managed_kind(7)     gui_legacy.cpp:8445
  -> launch_app_impl(app)        gui_legacy.cpp:8502, 8596-8599   [mkind == -2 分支]
  -> mforms_open_standalone      mforms.cpp:1505-1540
       clr_load_slot(slot,mex) -> clr_call("App.Main::Init") -> mforms_set_window_slot
  -> create_window / mforms_title / render_all   gui_legacy.cpp:8606-8625
  -> Paint / Click / Key 按槽分派  mforms.cpp:1607-1624 / 1664-1670 / 1699-1705
  -> 关闭窗口 -> mforms_close    mforms.cpp:1575-1582（当前不释放槽）
```

旁路：右键「用记事本打开」/ 文件关联走 `Shell.OpenNotepadFs` → `Host.OpenApp` → `launch_app`（`Desktop.cs:2979, 3073-3082`；`Shell.cs:471-476`；`gui_legacy.cpp:9594, 9838`），最终汇入同一 `launch_app_impl`。

---

## 7. 验证方法

- **串口标记**：成功应見 `[gui] opening standalone app`、`[MFORMS] standalone app live in slot 1`；失败为 `[MFORMS] standalone load failed` / `App.Main::Init failed` / `[gui] standalone open failed; falling back to the shell`（`gui_legacy.cpp:8597-8601, 8627`；`mforms.cpp:1511-1529, 1536-1538`）。
- **判定要点**：窗口标题必须来自独立 mex 的 `Title(id)`（修 B2 后才可能），而不是 shell 内置 `NotepadApp`；标题格式见需求文档 §1。
- **回归顺序**：构建 → 无头启动抓 serial → 桌面双击 → 输入/保存 → 关闭拦截 → 多开窗口 → 失败降级（临时移走 mex 验证回退路径）。
- **纪律**：每阶段改动都需实测，不能只编译通过；完成先出「验证问答」，确认后再进下一阶段。

---

## 8. 阶段排期

| 阶段 | 内容 | 主要文件（预计） | 完成判据 / 回滚点 |
|---|---|---|---|
| **S0 基线冻结** | 确认并行编辑已停；记录 `Notepad.mex` / `shell.mex` 的 size+mtime；锁定需求文档版本 | 无代码改动 | 有可复现的基线记录；若规格仍在变，暂停 S1 |
| **S1 修通单实例** | B1 + B2 + B4 + B7：注册表初版、Title/Close 分派、构建链接入、契约成文 | `mforms.cpp`、`gui_legacy.cpp`、`Makefile`、交互文档 | 双击图标打开的是独立 mex 窗口，标题正确，serial 出现 `standalone app live in slot 1` |
| **S2 生命周期** | B3：槽分配/释放、`id` 索引的每窗口状态、关闭拦截 | `mforms.cpp`、`clr.cpp`、`Program.cs` | 开两个窗口互不干扰；关闭后槽可回收；`clr_close_slot` 有真实调用点 |
| **S3 第二个应用** | 建议 **Calculator**（无文件 IO、交互最简单）验证模板可复制；顺带修 B5 / B6 | `csharp/apps/mex/Calculator/`、`Desktop.cs`、`Makefile` | 两个独立 mex 可同时运行；桌面/Start 自动列出两者 |
| **S4 批量迁移** | 剩余 10 个 stub 按复杂度排序逐个替换，每个单独提交 | `csharp/apps/mex/*`、`sfs_files/*.mex` | 每个应用都有 dotnet → mex 构建规则，且能从桌面打开 |
| **S5 清理** | 删除 shell 内置副本与 115 KB stub，收窄 `Apps.cs` / `Desktop.cs` | `Apps.cs`、`Shell.cs`、`sfs_files/` | shell 只负责桌面/窗口管理，不再包含任何 app 实现 |

> 每阶段独立提交、独立验证；任一阶段失败即停在该阶段，不跨阶段并行改动。

---

## 9. 风险与约束

- **并行编辑**：`NOTEBOOK_INTERACTION_SPEC.md` 在取证期间从 v1 变为 v2.0（17:03），`sfs_files/shell.mex` 11:12 更新。写实现前必须重新确认基线，避免覆盖他人 WIP。
- **槽容量**：`CLR_MAX_VMS=4` → 同时最多 3 个应用；需求文档 v2.0 §0/§1 要求「多实例且完全隔离」，与当前单槽单映像直接冲突，**需要用户决策**（见 §10）。
- **编号不一致**：`mforms.cpp:1440, 1507` 允许 `slot < 8`，`clr.cpp:1424` 强制 `slot < 4`；`mforms.cpp:1493` 注释写 AppType 6、`:1495` 写 7，真实为 11。
- **MiniCLR 限制**：无 Collections、无字符串 `+`、无静态初始化器、Paint 内分配不持久、icall 只返回标量 → 交互复杂度必须按能力裁剪。
- **Title 字符串生命周期**：`mforms_title` 用 `clr_heap_mark/reset` 包裹（`mforms.cpp:1584-1593`），按槽分派后需保持同样语义。
- **未接通 API**：`Host.DownloadFile*` 返回 -1（`mforms.cpp:995-1017`）；独立应用无 `W.Voice`、无共享窗口菜单（`Forms.cs:1173-1181, 1360-1366`）。
- **文档双源**：需求规格（v2.0）与实现契约（本轮新增）并存，需在 §10 确认归属，避免两份互相打架。

---

## 10. 待确认（需要拍板后才进入 S1）

1. **多实例策略**：① 一应用一槽、单窗口（最简单，与 v2.0 冲突，需改需求）；② 扩 `CLR_MAX_VMS` 并做「一窗口一槽」；③ 一应用一槽 + 槽内按 `id` 维护多份文档（推荐，省槽）。
2. **下一个 mex 化应用**：默认建议 Calculator；若你指定的应用不同，S3 排期与交互文档模板要相应调整。
3. **是否保留 shell 内置副本**（`Apps.cs:2540` 等）作为失败回退；若保留，B1 的 fallback 应回到 shell 而不是 native 抽屉。
4. **文档归属**：交互契约独立成文，还是合并回 `NOTEBOOK_INTERACTION_SPEC.md`（该文件正被并行编辑）。
5. **桌面策略**：是否继续只显示 Notepad（当前 `Listed()`），还是随 mex 上线自动放开。
6. **槽上限**：维持 4 还是扩到 8；这决定 B3 的实现方式。
7. **实现起点**：确认按 S0→S1 逐阶段推进、每阶段先出验证问答再继续。
