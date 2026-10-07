# NexOS 32 位镜像 V86 集成 — 阶段报告

> 目标：把 NexOS 的 32 位镜像通过 V86 跑在浏览器里，能进入命令行并执行 `help`。
> **结论：已达成。** 截图见 `docs/shots/v86-shell.png`，集成代码在 `web/v86/`。

---

## 1. 结果

浏览器里通过 V86（WASM）启动 32 位 NexOS 内核，登录后 `help` 正常输出命令表：

```
gui      Enter graphical desktop (VBE framebuffer)
ESC      Exit GUI mode and return to text
meminfo  Show memory/PMM/VMM/heap stats
memtest  Run kmalloc/kfree + PMM tests
pagatest test virtual memory page mapping
shutdown Power off (ACPI)
reboot   Restart system
...
PS root@minos:/_
```

登录：`root` / `admin`。启动约 20–40 秒（WASM 模拟 + 512 次单扇区读）。

参考：[V86](https://github.com/copy/v86)

---

## 2. 交付内容

| 路径 | 内容 |
|------|------|
| `web/v86/index.html` | 页面：加载 BIOS 与磁盘镜像，VGA 画面 + 串口输出接到 DOM |
| `web/v86/build-image.js` | 组装 `nexos32.img`（4 MiB），逐区校验，写入 MBR 分区表 |
| `web/v86/patch-image.js` | 按 LBA 精确改写镜像（调试用） |
| `web/v86/fetch-assets.js` | 拉取 `libv86.js` / `v86.wasm` / `seabios.bin` / `vgabios.bin` |
| `web/v86/README.md` | 使用说明与改动原因 |
| `web/v86/nexos32.img` | 组装产物（4 MiB，已提交，可直接打开页面） |
| `bootloader/stage2_v86.asm` | 面向 V86 的 Stage 2 |
| `bootloader/tools/probe_int13.asm` | `INT 13h AH=42h` 读取探针（定位问题时用） |
| `bootloader/stage2.asm` | 修掉一个与 V86 无关的真实缺陷（见下） |

---

## 3. 找到并修掉的两个问题

### 3.1 `stage2.asm`：`ESI` 未跨 `INT 13h` 保存（真实缺陷）

内核内扇区偏移保存在 `ESI`，但 DAP 指针必须放进 `SI` 才能调 `INT 13h`，
而 BIOS 有权破坏 `SI`。原代码没有保存，于是：

- 第一次读盘正常；
- 之后 `ESI` 变成 DAP 的地址，**每次读都指向错误的扇区**。

在 V86 下表现为第二次读盘返回 `AH=0x01`（"功能号无效"），
而**用完全相同的参数单独读一次却成功** —— 这一点曾让排查绕了很久。

修复：在 `int 0x13` 前后 `push esi` / `pop esi`。

> 复现要点：`INT 13h` 会在 `SS:SP` 留一个返回状态字节。若在中断前后做
> `push`/`pop`，`pop` 拿到的是那个状态字节，会让 32 位目标寄存器的高 16 位
> 变成 `0x0000` —— 调试时曾被这一点误导，误判"指针被改写"。

### 3.2 `stage2.asm`：段步进硬编码（真实缺陷，与 V86 无关）

```asm
add word [dap_kernel + 6], 0x0800            ; 段 += 32KB
```

`0x0800` 只对 `KERNEL_CHUNK = 64` 成立。任何调整分块大小的改动都会让内核在加载
时**自我覆盖**。已改为由 `KERNEL_CHUNK` 推导：

```asm
KERNEL_SEG_STEP    equ KERNEL_CHUNK * 32
```

### 3.3 V86 目标专用 Stage 2 的三处差异

`bootloader/stage2_v86.asm`：

1. `ESI` 跨 `INT 13h` 保存（同 3.1）；
2. **每次只读 1 个扇区**，读取路径不依赖跨调用的寄存器状态；
3. **不设置 VBE 图形模式**。内核若认为图形模式就绪会去初始化 GUI，
   在 V86 下触发 `#GP`；而 **V86 未实现 `#GP` 处理，会直接 panic
   整个模拟器**（`panicked at cpu.rs: Unimplemented: #GP handler`）。
   本目标只需要命令行，保持文本模式即可。

### 3.4 镜像需要分区表

`build-image.js` 在 MBR 偏移 446 写一个覆盖全盘的分区表项。
V86 靠分区表推导模拟磁盘几何（`get_disk_geometry`）；没有分区表时
BIOS 经 `INT 13h AH=08h` 返回**全 0 几何**。

---

## 4. 已排除的假设（都有对照实验）

| 假设 | 结论 |
|------|------|
| 镜像太小 / 几何不对 | 排除：1 MiB→4 MiB 均复现 |
| V86 不支持 `INT 13h AH=42h` | 排除：单次读任意 LBA 均成功 |
| 单次读太大 | 排除：1/8/16/24/32/40/48/56/64/128 扇区全部成功 |
| 目标段不可写 | 排除：`0x1000`–`0xA100` 全部成功 |
| 读盘次数 / 重复读 | 排除：相同读连做 3 次全部成功 |
| DAP 内容非法 | 排除：`int 0x13` 前后逐字节 dump，与标准模板一致 |
| 汇编/机器码有误 | 排除：反汇编核对 `mov ah,0x42` / `int 0x13` / DAP 地址 |
| `AH=08h` 几何为 0 | **是症状之一**，已用分区表解决 |
| 缺少 CD/软盘引导 | 不需要：硬盘路径可用 |

---

## 5. 已知限制

- **无图形界面**：`gui` 命令在 V86 下不可用（`#GP`）。命令行功能正常。
- **无 SFS 文件系统**：镜像未含 `sfs.img`，内核提示 `SFS: not found`；
  命令行基础功能不受影响。
- 启动较慢：512 次单扇区读 + WASM 模拟。

---

## 6. 本地验证环境

| 项目 | 说明 |
|------|------|
| 浏览器 | Microsoft Edge（无头模式，Puppeteer 驱动） |
| 汇编器 | NASM 2.16.03 |
| 串口验证 | 内核全流程写 `0x3F8`，无头下可直接读串口，无需 OCR |
| 键盘注入 | `emulator.keyboard_send_scancodes()`（内核读 PS/2，非串口） |

复现命令：

```bash
cd web/v86
node fetch-assets.js
node build-image.js
python3 -m http.server 8000     # 然后浏览 http://127.0.0.1:8000/
```
