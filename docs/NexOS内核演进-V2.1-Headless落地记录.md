# NexOS 内核演进 V2.1 — Headless 阶段落地记录

| 项目 | 内容 |
|---|---|
| 日期 | 2026-11-13 |
| 战略定位 | 以**硬件隔离、抢占调度、AI 原生能力**为基石的微内核级操作系统 |
| 本轮范围 | 草案「近期行动拆解」Task 1 / 2 / 3：无头旁路 → `hlt` → 硬件时钟中断 → 双内核线程上下文切换 |
| 状态 | 代码完成，实机验证进行中（结果见第 5 节） |
| 关联文档 | `docs/内核现状评估与模型外置方案.md`（现状与差距）、`docs/NexOS_API_升级总设计.md`（OS API 契约） |

---

## 1. 本轮为什么这么做

V2.1 的目标是「搁置界面，直击内核」。此前内核的全部能力（GUI、PE 兼容层、AI 推理、OS API）都跑在 ring0、同一个 identity-map 地址空间、同一个**不 `sti`** 的轮询主循环里。缺的是三条机制：隔离、抢占式调度、资源虚拟化。

本轮不动 GUI / PE / AI 的任何代码，只做一件事：**让 CPU 在开中断的状态下，由时钟中断驱动，在两个独立上下文之间切换**。这是后续一切（ring3、独立 PML4、按需分页、AI 沙盒）的前置条件。

---

## 2. 交付内容（按文件）

| 文件 | 改动 |
|---|---|
| `Makefile` | 新增 `HEADLESS_STAGE ?= 0`；**只**在 `build/kernel64.o` 的编译行注入 `-DNEX_HEADLESS_STAGE=$(HEADLESS_STAGE)`（不进 `CXX64FLAGS`，否则所有 64 位对象都带不同的命令行，make 还不会察觉，会静默混编不同 stage 的对象） |
| `.attic64/entry64.asm` | 末尾新增 `irq0_stub`：保存 15 个 GP 寄存器 → 切到专用 8 KiB IRQ 栈 → 调 C 调度器 → 用返回值重装 `rsp` → 发 PIC EOI → 恢复寄存器 → `iretq`；`.bss` 里新增 `hs_irq_stack` |
| `.attic64/kernel64.cpp` | 新增 headless 段落（约 170 行）：无锁 raw 串口、PIC 重映射、PIT 100 Hz、`HSThread` PCB、伪造 iret 帧、`headless_irq0_c()` 调度器、`headless_run()`；`build_idt()` 为向量 32 换 gate；`kmain64` 在 AUTOTEST/login/cmd_gui **之前**插入旁路 |
| `tools/rebuild_headless.sh` | 增量构建脚本，参数即 stage；`MODEL_GGUF` 指向不存在的路径（见第 3.5 节） |

### 阶段语义（编译期常量，零运行时开销）

| stage | 行为 |
|---|---|
| **0** | **完全不变的旧 GUI 路径**（硬约束 4 的生路）。所有 headless 代码仍编译，只是不执行 |
| **1** | 跳过 GUI / 登录 / 模型 AUTOTEST；显式跑 L0–L12 OS API 自测；`cli; hlt` 死循环 |
| **2** | + 8259A 重映射 32..47、PIT 100 Hz、只 unmask IRQ0、`sti`；中断里节流打印 `[TICK]` |
| **3** | + 两个各有 4 KiB 私有栈的内核线程，时钟中断轮转，交替打印 `A` / `B` |

---

## 3. 六个关键设计决策

### 3.1 所有代码无条件编译，只有行为分支
如果按 `#if NEX_HEADLESS_STAGE >= 2` 把 `headless_irq0_c()` 裁掉，stage 0 时 `entry64.o` 仍引用它，`check-k64` 的严格重链就会报未定义符号。所以：**符号集在所有 stage 恒定**，分支用 `if (常量)` 让编译器折叠。实测 `HEADLESS_STAGE=3` 下 `k64 undefined: 0 (clean)`。

### 3.2 中断必须发 EOI 且必须 `iretq`，不能走 `fault_common`
`entry64.asm` 原有 256 个 stub **全部**汇入 `isr_common → fault_common()`，而后者 `cli;hlt` 永不返回。硬件 IRQ 若落到那里就是死机。因此向量 32 的 gate 单独指向 `irq0_stub`（`0x8E00`，interrupt gate，IST=0 —— IRQ 不是故障，必须留在被中断线程的栈上）。

### 3.3 PIC 重映射是必须项，不是可选项
`kmain64` 开头把两片 8259A 全屏蔽并 `cli`，注释里记录了根因：PIC 复位态把 IRQ0 投递为向量 **0x08 = #DF**，一个时钟滴答就会连锁三重故障。所以 `hs_pic_remap()` 用 ICW1–ICW4 把主片映射到 32、从片映射到 40，然后 `0x21=0xFE`（只放行 IRQ0）、`0xA1=0xFF`。

### 3.4 专用 IRQ 栈 + 无锁串口，防两处死锁
- 同特权级中断门只压 `rip/cs/rflags`。若直接 `call` C 函数，C 的返回地址和局部变量会**覆盖 CPU 的 iret 帧**。所以 stub 先把 `rsp` 换到 `.bss` 里的 `hs_irq_stack_top`，再把被中断帧地址当参数传给调度器。
- `serial_puts()` 用 test-and-set 自旋锁。线程持锁打印时被时钟中断、中断里再拿同一把锁 → 在 `IF=0` 下永久自旋。因此中断/线程上下文一律用 `hs_puts()`：直接 `outb(0x3F8)`，**无锁**。

### 3.5 跳过 GUI 也必须跑 OS API 自测（硬约束 2）
`mforms_init()`（内含 `osapi_init()` + 全部自测 + icall 注册）原本只在 `gui_enter()` 里调用。旁路 GUI 会让 L0–L12 的 `fails=0` 全部消失。因此 `headless_run()` 显式调用：
`osapi_init / self_test / l2l3 / proc×2 / cfg / mem×2`（纯 C，不碰 CLR 字符串，延续此前「自测假绿」的教训）。

注意 `[SHELL] ... ok=1` 是 `shell.mex` 托管侧自测，只有 `mforms_start()` 才会执行；headless 下它按设计**不运行**，验收以内核侧 `[OSAPI] ... fails=0` 为准。

### 3.6 本轮构建不嵌模型（顺带迈出「模型外置」第一步）
`MODEL_GGUF=build/__no_model__.gguf`（不存在）→ `embed_model.py` 不执行，镜像从 **429 MB 降到约 30 MB**。收益：不再向共享文件夹写 400 MB（上一轮正是它把 Linux 构建 VM 写挂）、VDI 同步从分钟级降到秒级；同时第一次实证了**内核启动不依赖那 397 MB 权重**。正式的外置方案（数据盘高 LBA + 懒加载）见 `docs/内核现状评估与模型外置方案.md`，仍待实施。

---

## 4. 与草案 V2.1 的差异（必须明确，不算完成）

| 草案条目 | 本轮状态 |
|---|---|
| 启动旁路 `g_headless_mode` + `hlt` | ✅ 以编译期 `HEADLESS_STAGE` 实现（草案允许「编译开关」） |
| 时钟中断 + 抢占式调度 + idle | ✅ PIT 100 Hz + 两内核线程轮转；**PIT/8259A 是草案允许的过渡路径**，LAPIC Timer / x2APIC 未启用 |
| `task_switch(old_ctx,new_ctx)` + A/B | ✅ 由 `irq0_stub` + `headless_irq0_c` 协作完成，仍在 ring0 |
| 废除恒等映射 / 独立 PML4 / #PF 按需分页 | ❌ **未做**。当前仍是 identity map，属于下一阶段 |
| Ring 3 用户态 / syscall 门 / 独立进程 | ❌ 未做（草案也标为「未来阶段」） |
| ATA 异步化 / 中断驱动块设备 | ❌ 未做 |
| 模型移到数据盘 + `mmap` 按需加载 | ⚠️ 仅做到「不嵌进系统盘镜像」，正式外置与 mmap 未做 |
| 软硬件分离 / 驱动移出内核态 | ❌ 未做，本轮是**最小侵入式旁路**（代码集中在一个段落，便于后续拆成独立模块） |

> 一句话：本轮只把「CPU 在中断控制下准时跳动」这一件事做实。隔离、虚拟内存、用户态仍是 V2.2+ 的目标。

---

## 5. 验收与实测（2026-11-13 16:10，VBox NexOS，stage 3）

**串口关键行**（`build/vbox_serial.log`）：

```
[K64-IDT] vector 32 -> headless irq0_stub
[K64-IDT] IST1 fault stack at 003AA700
[K64-COMPAT] armed
[KERNEL] headless stage=3
[OSAPI] self-test done, fails=0
[OSAPI] l2l3 self-test fails=0
[OSAPI] proc/glob self-test fails=0          (×2)
[OSAPI] cfg self-test fails=0
[OSAPI] mem self-test fails=0                (×2)
[OSAPI] L4 gen++ proof: slot=0 slotgen 1->2 handlegen 6->7 stale=21
[KERNEL] OS API self-tests done (headless)
[KERNEL] Entered headless hlt loop.
[TICK] ...  A  B  A  B ...
```

| # | 硬约束 / Task | 实测结果 |
|---|---|---|
| 1 | `check-k64` 未定义符号 = 0 | `k64 undefined: 0 (clean)`，stage 3 与 stage 0 各跑一次严格重链，均通过 |
| 2 | 新增代码零警告 | 编译输出的 warning 全为存量（233/712/937/2633/3158/8759… 行），headless 段落无警告；`MAKE_EXIT=0` |
| 3 | GUI / PE / mforms 的 `.o` 仍全部保留 | 未改 `KERNEL64_OBJS`；`os_v2.img` 里 Kernel64 = 1,145,680 B |
| 4 | Task 1：串口标记 + CPU 不 100% | `[KERNEL] Entered headless hlt loop.` 出现；NexOS 主 VM 进程 5 秒 CPU 增量 **0.297 s = 5.94% 单核**（宿主另一个 84% 的 VBoxHeadless 是 15:01 启动的 Linux 构建 VM） |
| 5 | Task 2：硬件中断打破 `hlt` | `[TICK]` 持续打印，55 秒采样 56 次；节拍由 PIT 100 Hz 驱动 |
| 6 | Task 3：A/B 交替 | **A=113 / B=113**，55 秒采样，严格交替；期间无死锁、无三重故障 |
| 7 | Headless 下 L0–L12 自测 `fails=0` | 全部 `fails=0`。**额外收获：上一轮挂起的 L4 内存层实机验证在本轮补完**，`gen++ proof` 两次都打印且 `fails=0` |
| 8 | stage 0 回退生路 | 编译 + `check-k64` 均通过（`k64 undefined: 0 (clean)`）；**未实机启动 GUI**（当前 VDI 是 stage 3 镜像），下一轮可 `bash tools/rebuild_headless.sh 0` 实测 |
| 9 | 防死锁 | 无锁串口 + 专用 IRQ 栈；唯一的循环是 idle `hlt`，靠中断唤醒；55 秒持续交替证明未卡死 |
| 10 | 模型解耦 | `Model: (none …)`，镜像 **429 MB → 31,063,040 B（29.6 MiB）**；`vbox_check.py` → `IN SYNC`（系统盘 + 数据盘） |

**修掉的一个真实缺陷**：初版 `g_hs_credit` 周期为 20（偶数），而线程轮转周期为 2 —— 每次发牌都落在同一个线程上，串口只出现 `B`，A 为 0。改为奇数周期 25 后 A/B 严格交替。这类"周期同相"问题只在实机串口统计里看得出来。

**已知噪音（非失败）**：`[CLR] fault: no file reader bound` ×2。headless 没有走 `gui_init()`，因此 MiniCLR 未绑定文件读取器；`proc/glob` 自测尝试 spawn 时打印该行，但自测仍 `fails=0`。后续可在 headless 初始化时绑定一个只读 file reader 消除。

---

## 6. 操作手册

```bash
# 构建 VM 内（Linux），stage 3 = 完整验证
bash tools/rebuild_headless.sh 3
# 或 stage 2（只验时钟）/ stage 1（只验 hlt）/ stage 0（回退 GUI）
bash tools/rebuild_headless.sh 2
bash tools/rebuild_headless.sh 0

# Windows 宿主：同步 VDI 并启动
powershell -ExecutionPolicy Bypass -File tools\vbox_sync.ps1 -VmName NexOS
VBoxManage startvm NexOS --type headless
# 串口日志：build/vbox_serial.log
```

**注意**：`vbox_sync.ps1` 会把当前 `build/os_v2.img` 转成 VDI。只有在 `MAKE_EXIT=0` 且镜像完整时才能同步；残缺中间态同步会毁掉可启动版本（上一轮已踩过）。

---

## 7. 后续路线（建议顺序）

1. **V2.1 收尾**：把 headless 段落拆成独立 `headless.cpp` / `headless_asm.asm`（软硬件分离），补 APIC/LAPIC Timer 替换 PIT+PIC。
2. **V2.2 虚拟内存**：每进程 PML4、#PF 处理、按需分页；内核与用户态地址空间分离。
3. **V2.3 用户态**：ring3 + syscall 门 + capability 校验，先把 `Mem` / `File` 的 OS API 从这里走通。
4. **模型正式外置**：数据盘高 LBA + `mmap` 按需加载，AI 推理迁移到独立服务（AI 沙盒）。
5. 每一步都复用 `HEADLESS_STAGE` 的分级验证思路：新机制一律以**可回退的显式开关**接入，默认路径保持上一次实机全绿的状态。
