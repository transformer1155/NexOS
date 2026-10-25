# NexOS 图形栈调试 Playbook（专职: glm-53）

> 本文件是 gui/渲染 bug 排查的完整方法论。所有结论必须落到 gui.cpp/mforms.cpp/*.cs 的具体行号，
> 禁止"可能是缓存/时序问题"式空话。每次修完必须 QEMU 截图 + Python 像素验证，禁止只看代码就宣布修好。

## 架构速查（两层合成模型）
- **原生合成器**: gui.cpp `Win11Desktop::render_all()`（约 L7411 起）。分层:
  桌面层(壁纸+图标, retained 缓存 g_wall) → 窗口循环(draw_window_damaged, 每次调用全量重绘) →
  overlay(mforms_paint_overlay: 任务栏/Start菜单/Popup) → 光标 → damage 链 present。
- **托管壳层**: C# NexOS.Forms 经 mforms_* 桥。C++ 看不到 C# 的 menuOpen/Popup 状态，
  只能靠 clr_call 探测（mforms_desktop_menu_open 等）。
- **两个独立的门**:
  门A overlay重绘: `over_dirty = repaint_desk || g_mforms_anim || g_over_force_n>0 || g_over_idle>=N || osig!=g_over_sig` (L~7698)
  门B present: 只有 dirty 链里的矩形才 present_rect 到 LFB (L~7891)。
  **重绘了 ≠ present 了**。两个门各自独立出 bug。

## 已修复的 bug 模式（commit f04f011, dcbbe36, 0f0494d）

### 模式1: "只 dirty 不重绘" → 光标重影
dirty_add 只是 present 标记，不代表 backbuffer 像素被重写。L7715 的全屏 flush 和
BUG-C1 的窗口补标都是"只标记不重写"。光标块的 under_covered 若信任它们，
save_bg() 会把上一帧光标烤进快照 → 永久幽灵。
**判据**: 只有桌面层全量重绘（render_all 内 bg_repainted_this_frame=true 处）才允许跳过 restore_bg。

### 模式2: 签名盲区 → 菜单冻结
osig 组成: running_mask ^ start_menu_open ^ hover-hash。**C# 的 menuOpen/Popup 不在其中！**
managed 菜单打开后 osig 不变 → 门A 关闭。叠加:
- g_over_force_n=6帧(96ms) < C#动画170ms → force 窗口先耗尽
- gui_tick 只在 g_mforms_anim 时驱动 render_all → anim=0 后空闲帧归零 → g_over_idle 兜底永不触发
**修法**: osig ^= menu_up; force=12帧; 2Hz keepalive; idle 阈值降到 2。

### 模式3: lock screen 挡住一切 (dcbbe36)
auto-GUI boot 后 Login.IsActive()==1 但无人登录 → Shell.cs 所有入口静默 return。
**诊断铁律**: 桥接返回 cc=0 ret=0 不代表 C# 干了活，必须分层加 Host.Log 确认。

### 模式4: SetAnim 生命周期
h_set_anim 是直接赋值（最后一次调用赢）。同一 clr_call 事务里 PaintOverlay 尾部的
SetAnim(1) 可能被后续 SetAnim(0) 覆盖 → gui_tick 停摆。

## QEMU 验证标准流程（tools/verify/verify_fixes.sh 已固化）
1. 构建: WSL Ubuntu-24.04。C# 改动必须 `rm -f sfs_files/shell.mex` 再 make（Makefile wildcard 陷阱）。
   C++ 改动 rm 掉对应 .o。clock skew 警告可忽略。
2. 启动: `-drive format=raw,file=build/os_v2.img -m 4096 -vga std -display none
   -serial file:_qemu_serial.log -monitor unix:/tmp/mon.sock,server,nowait -no-reboot`
   **不许 -snapshot**；镜像被锁时先 kill -9 所有 qemu。
3. 输入注入（monitor 命令，通过 unix socket）:
   - `mouse_move dx dy` 是**相对**移动！每次≤80px，自维护 CURSOR 累计坐标。
   - `mouse_button N`: 1=左 2=右 0=全部释放。
   - `screendump file.ppm` 后 sleep ≥2.5s。
4. Python 判定（tools/verify/verify_diff.py / ghost_check.py）:
   - 菜单出现: diff > 15000 px（菜单背景 RGB sum ~645，壁纸 ~282）
   - 重影: 在长按点/光标路径点查 40-52px 盒，<50 px 差异 = PASS
   - 帧对比必须 base/menu/after 三帧取齐，先 dismiss 再采 base。
5. 串口诊断标记: [GUI-STEP] [MFORMS] [CLR] [LOGIN] [RF] [DR] — 桥接断点逐层加
   serial_putdec（gui.cpp/mforms.cpp 各有 static 版本，**不是** serial_puts_dec）；
   C# 用 Host.Log(U.Cat(..., U.I(n)))，**不支持 string+int 拼接**。

## 已知环境陷阱
- 分支是 main；remote origin=gitee, github=github。Team lead 负责提交。
- QEMU monitor mouse 命令在 8.2.2 是 mouse_button/mouse_move，不是 "mouse button right"。
- MiniCLR: 无 closures/generatics/floats/try-catch；U.Cat 可变参全 string（int 要 U.I()）。
- os_v2.img 布局: kernel64@LBA2048, sfs@LBA6144（superblock cand={800,6144,3536}）。

## 待办/观察项
- 右上角 3.4k px 的帧差是时钟秒数+托盘（合法），写判定脚本时排除 (x>1140, y<60)。
- 若实机 [CURSOR] backend 非 software，重影分析需重做（硬件光标路径未覆盖）。
- Popup 悬在纯壁纸上且无可见窗口时，BUG-C1 补标为空集，其 flip 依赖 L7715 分支 —
  未来若改窄 L7715 需保留兜底。
