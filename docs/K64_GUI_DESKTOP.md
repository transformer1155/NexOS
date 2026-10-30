# 64 位桌面 GUI — 根因与验证记录

状态：**已解决**（2026-10-30）
结论：**不需要改任何代码** —— 根因就是 allowlist 里最后一个符号 `svga_mode_hint` 被绑到地址 0。

---

## 1. 最初报告的症状

64 位内核文本控制台完全正常，但一进入桌面就彻底静默：

```
[K64] Entering Win11 GUI mode
[DIAG] step=333 gui_enter starting
<再无任何输出>
```

并且 `gui` 之后的三次 `screendump`（+5s/+20s/+50s）**全部失败**，一张帧都没产出。

当时有两个**错误的推断**，都被后续实测推翻：

| 错误推断 | 真相 |
|---|---|
| "gui_enter 把显示面切成 QEMU 无法 dump 的状态" | **VM 已经整个退出**（三重点 + `-no-reboot`），所以 dump 失败 |
| "GUI 侧是独立问题，与符号债务无关" | 它就是债务清单里**最后一条**的可见症状 |

---

## 2. 真正根因

`gui_enter()` 在 `diag_step(333, "gui_enter starting")` 之后的**第一条实质调用**是：

```cpp
// gui.cpp:11445-11449
diag_step(333, "gui_enter starting");
svga_mode_hint(g_wm.gfx.width, g_wm.gfx.height, g_wm.gfx.bpp, g_wm.gfx.pitch);
```

当时 `svga_mode_hint` **仍在 allowlist 里**（定义在 `svga.cpp:194`，未编入 64 位链接），
被 `--unresolved-symbols=ignore-all` **静默绑到地址 0**。于是：

```
call 0  ->  执行实模式 IVT  ->  #UD  ->  #GP  ->  #DF  ->  三重点
```

在 `-no-reboot` 下 QEMU **进程直接退出**，这同时解释了：

- 串口停在 `step=333`（后面再也写不出）
- 三张 `screendump` 全部失败（不是 surface 不可 dump，**是 VM 没了**）

---

## 3. 修复

**已由 task-18 的 D3 完成**（提交 `0fde327`）：新增 `$(BUILD)/svga64.o` 规则、
`svga_mode_hint` 真实现（`svga.cpp` 原样 `-m64` 编译），并解决了 `vmm_map_page`
同名不同链接属性的冲突（旧的 32 位两级映射器改名 `vmm_map32_page`）。

**本轮没有改任何代码** —— 纯粹是诊断与验证。

---

## 4. 验证（Lead 独立复现）

启动序列：`build/os_textboot.img` + `qemu-system-x86_64` + 32 位登录 `root/admin` `switch`
→ **64 位再登录 `root/admin`** → `gui`。

### 串口（关键段）

```
[CLR] MiniCLR initialised (36 internal calls)
[K64] Entering Win11 GUI mode
[DIAG] step=333 gui_enter starting
[CURSOR] backend = software cursor (guest-painted)
[SPLASH] starting animation
[SPLASH] frame drawn  ×5
[SPLASH] model load begins
[BOOT] mforms_boot cycles=2744519842
[GUI] Entered GUI mode
[M-GE]                             <- gui_enter() 正常返回
[M-GR]                             <- gui_render() 正常返回
```

### 截图（5 张全部成功，各 2,764,816 B = 1280×720 RGB）

| 文件 | 阶段 |
|---|---|
| `a_32bit.ppm` | 32 位 GUI |
| `b_k64term.ppm` | 64 位文本控制台 |
| `c_k64afterlogin.ppm` | 64 位登录后 |
| `d_gui20.ppm` | `gui` +20s |
| `e_gui60.ppm` | `gui` +60s |

### 数值分析（不依赖人眼，可复现）

| 指标 | GUI 前（终端） | GUI +20s | GUI +60s |
|---|---|---|---|
| 平均 RGB | (65.6, 16.9, 123.2) | (32.5, 102.7, 173.5) | **(32.5, 102.7, 173.5)** |
| 采样颜色数 | **4**（纯终端） | **949**（丰富图形） | **949** |
| 非黑像素占比 | 95.9% | 100% | 100% |

- 颜色数 **4 → 949**：从纯文字终端变为带图形的桌面
- 平均色转为**蓝色调**（B=173.5），符合 Win11 蓝色壁纸
- **+20s 与 +60s 两帧像素完全相同（0.00% 变化）** —— 桌面**稳定渲染**，不是花屏/滚动
- 终端 → 桌面像素变化 **100%**（整屏重绘，符合预期）

---

## 5. 教训（比修复本身更重要）

### 5.1 「日志停住」不等于「没有输出」

`driver-network` 一开始用 **grep 关键字**筛选日志，把 `[DIAG] step=3xx mh_*` 这类
正常渲染节拍**滤掉了**，于是误判为"输出停止"。

**正确做法：定位停点要看原始尾部（`tail`），不要只 grep 关键字。**

### 5.2 「screendump 失败」要先确认 VM 还活着

`screendump` 失败有两种完全不同的原因：

- 显示面状态异常（VM 还在）
- **VM 已经退出**（三重点、panic、QEMU 被 kill）

判别方法：先看 QEMU 进程是否还在，或用 `-d int,cpu_reset` 抓异常。
本轮若一开始就检查进程存活，可以少走一次弯路。

### 5.3 弱链接把「符号缺失」伪装成「GUI 卡死」

`svga_mode_hint` 这条债务的症状是**桌面卡死**，而不是任何编译/链接错误。
这是 `--unresolved-symbols=ignore-all` 最危险的地方：
**缺失定义可以表现为任意远端的运行时症状。**

闸门（task-15）到 0 的价值正在于此 —— 第 43 条债务的可见症状几乎无法从现象反推回符号。

---

## 6. 遗留（已知，与本问题无关）

`[BOOT] mforms_boot cycles=2.7e9` —— 托管（C#）外壳加载耗时约数十秒量级，
期间屏幕是 splash 定格。**这是真实的体验问题**，值得单独立项，但与本次卡死无关。
