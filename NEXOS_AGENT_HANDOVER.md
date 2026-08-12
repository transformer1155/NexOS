# NexOS Bootloader — Agent 交接知识档案

> 本文件是 NexOS（原 MiniOS）bootloader 项目的**全部经验、坑点、架构与诊断方法**的完整汇总。
> 写给接手的另一位 agent 使用：读完即可独立继续本项目，无需向我回问。
> 最后更新：2026-08-11。项目根：`D:\MyOS\bootloader`（WSL 路径 `/mnt/d/MyOS/bootloader`）。

---

## 0. 一句话背景

NexOS 是一个**自研 x86 操作系统内核 + 32 位 C# 托管 GUI 壳**，支持 BIOS 与 UEFI 双固件引导，能跑真实 PE32 程序（含自研 IE 浏览器）。当前主线在修**真机（华为 i5-1155G7 / Iris Xe，UEFI）上的显示问题**：高帧缓冲（>4GB）黑屏已修，但**白条/白线仍待真机定性**。

---

## 1. 构建环境（铁律，违反必踩坑）

- **所有 Linux 构建一律走 WSL**：`wsl -e bash -lc 'cd /mnt/d/MyOS/bootloader && make <target>'`。不要用其他虚拟机/远程环境。
- 工具链（WSL 内）：`nasm` / `g++ -m32 -ffreestanding` / `ld` / `objcopy` / `qemu-system-x86_64` / `xorriso` / `mtools`（mcopy/mmd）。
- **Windows 侧 `rm` 被 safe-delete 拦截** → 清旧产物必须在 WSL 内 `rm -f`。
- **WSL/Windows 时钟偏移会让 `make` 跳过重编**（文件 mtime 比源新）→ 改完代码若构建"没生效"，先 `rm` 掉旧产物再 `make`。
- 统一内存 `-m 128M`。

### 主要 make 目标
| 目标 | 产物 | 用途 |
|---|---|---|
| `make`（默认，2026-08-05 起） | `build/os.iso` | CD 布局，主交付 |
| `make bios` | `build/os.img` | BIOS raw 镜像，LBA33=kernel.bin，无头测试 |
| `make uefi` | `build/os_uefi.img` | UEFI GPT 镜像（128MiB），真机主用 |
| `make uefi-diag [FB_TEST=1\|2\|3]` | `build/os_uefi_diag.img` | **白条诊断镜像，独立产物链，不污染 `uefi`** |
| `make play` / `make bios-run` | — | 看 C# GUI（GTK / BIOS 运行） |
| `make test-sec` | — | 安全套件（Foundation0/弹窗/Win32 GUI 无头） |

---

## 2. 架构（双架构 32+64 位）

- 32 位 `kernel.bin`（链接 `0x10000`，BIOS 镜像 LBA33）与 64 位 `kernel64.bin`（链接 `0x100000`，LBA2048）**同存** `build/os.img`；SFS 文件系统在 LBA3328。
- 架构切换：
  - 32→64：`switch64`（kernel.cpp ~5019）从 LBA2048 读 **1024 扇区**到 `0x100000`。
  - 64→32：`switch32`（kernel64.cpp ~2715）从 LBA33 读 **1024 扇区**到 `0x10000`。
  - **两方向 SECTORS 均=1024，勿改回 800/256**（历史踩坑）。
- VMM：4MiB PSE 身份映射前 32MiB + VBE LFB；VGA 空洞 `0xA0000–0xBFFFF`；用户态 `USER_BASE=0x04000000..USER_END=0x08000000`。
- **已修复的根因（勿再踩）**：
  ① `page_directory` 与 `.bss` 重叠 → 三重故障；现 `g_page_directory_store[1024]` 带 `alignas(4096)`。
  ② `.bss` 尾落 VGA 空洞 → 已 relocate 到 1MiB+。

### UEFI 启动链（关键标记，用于判读串口日志）
```
bootuefi.c (gnu-efi)
  → enter_kernel.S：不退出 long mode，切 IA-32e 32 位兼容模式，沿用 UEFI 4 级页表
  → 32 位 kernel.cpp
串口标记序列： ST → [GOP] → [GOPF] → [4] → [5] → EGI3SFB → [K1] → [K32] Entering Win11 GUI mode
```
`EGI3SFB` 来自 `enter_kernel.S`（32 位兼容模式切换完成、BSS 清零）。**若复位发生在 `EGI3SFB` 之后、GUI 之前 → 是 QEMU/OVMF 对 32 位 UEFI 内核早期分页的环境性不兼容，不是代码 bug**（VirtualBox 与真机 Iris Xe 能完整启动）。

---

## 3. GUI 纪律（C# 优先，用户 2026-08-09 强制）

- **「要改的图形界面」尽量只改 C#**（`csharp/`）：桌面/窗口/菜单/图标/托盘/右键菜单都在 32 位 C# 托管壳（`NexOS.Forms` + `apps/Shell/*`）。
- 品牌统一 **NexOS**（PascalCase）。新增 App 用 `NexOS.Forms` / `NexOS.Core`，**勿用** MiniOS / miniOS / nexos（历史重命名踩坑：大小写不一致会导致 `entry point not found: NexOS.Forms.Shell::Init`）。
- C++ 内核层（`gui.cpp` / `mforms.cpp` / `win32.cpp`）**只留**：host 回调注册、P/invoke 桥、ring-0 资源（fb/输入/窗口 surface）。不要在内核里写业务 UI。
- **C# 约束（CLR 移植版，非完整 .NET）**：无浮点 / 无泛型 / 无接口 / 无 try-catch；数组 4 字节槽；**静态初始化器不执行**（需 `Shell.Init()` 显式赋值）；堆每帧回卷；动画状态存 `static`。

---

## 4. 高频坑（已修，勿再踩）

| 坑 | 现象 | 现状 |
|---|---|---|
| `handle_mouse_move` 中 `cursor.move` 调两次 | 鼠标位移 ×2 | 只能调一次 |
| fbcon 清 LFB 抹 GUI | gui_mode 下 `term.render()` 调 `fb_console_render` 抹掉界面 | 已加 `if(g_wm.gui_mode) return;` |
| 退出 GUI 方式错 | `disable_vbe` 后 fbcon 不恢复 | 用 `gui_exit()` 保持 BGA+fbcon，并 `fb_console_force_redraw()` |
| 16bpp 颜色掩码 | 颜色错乱 | `((color>>19)&0x1F)<<11 \| ((color>>10)&0x3F)<<5 \| (color>>3)&0x1F` |
| 32 位内核未注册 GUI 回调 | `g_cb` 全零、GUI 不起来 | `kmain` 必须 `register_gui_callbacks()`（cmd_gui） |
| C# 控制键负向虚拟键 | 64 位退格/回车失效 | `0x08→-1`(Backspace)、`\n`/`\r`→`-2`(Enter)；C# `OnKey` 须同时接受负向与原始 ASCII |
| CLR 字符串字面量上限 | `[CLR] fault: too many string literals` | `clr.cpp` `CLR_MAX_STRINGS` 256→512；加 App 报此错继续上调（同扩 `g_strobj[]`） |

---

## 5. 测试 / 验证工作流（用户铁律：必须实测）

- **用户要求**：所有改动完成后**必须实际运行验证**（引导/截图/串口输出）才能交付，编译通过 ≠ 完成。
- 无头测试脚本：`tools/test_ie_pe.py` / `test_ie_click.py` / `test_desktop_ui.py` / `test_kb_input.py` / `test_linux_compat.py`（4451）。跑完 `pkill -f "[q]emu-system"`。
- **验证通道分工**：`term.write` 不写串口 → 屏幕内容靠 **screendump（截图）**；内核逻辑靠 **`serial_puts` 串口日志**。
- **VirtualBox 真机式引导**（Windows 侧，`VBoxManage`）：
  - BIOS：`MyOSTest`（32-bit, 128MB, PIIX3, VBoxVGA, COM1→文件）；`VBoxManage convertfromraw build/os.img build/os_vbox.vdi`。
  - UEFI：`MyOSUefi`（64-bit, EFI64, 128MB, PIIIX4）。
  - `.vdi` 重建后 UUID 冲突 → `VBoxManage internalcommands sethduuid <vdi> <old_uuid>` 修复。
- **QEMU + OVMF**（WSL 内，用于 UEFI 冒烟）：OVMF **必须用 pflash 加载**（`-bios` 失败）：
  ```
  qemu-system-x86_64 -m 128 -machine q35 \
    -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE.fd \
    -drive if=pflash,format=raw,file=build/ovmf_vars.fd \
    -device qemu-xhci -drive if=none,format=raw,file=build/os_uefi.img,id=usb -device usb-storage,drive=usb,removable=on \
    -serial file:build/serial.log -display none
  ```
  - UEFI 可移动介质自动启动**只认 `BOOTX64.EFI`**（EFI 路径 `EFI/BOOT/BOOTX64.EFI`）。非标准名无法自动引导。
  - 若 OVMF 落到 EFI Shell，临时注入 `startup.nsh`（`EFI\BOOT\BOOTX64.EFI`，相对路径、无前导反斜杠）自动启动；**不要改交付镜像本身**。

---

## 6. Win32 PE 加载器

- `run <file>` → `cmd_run` → `winloader.cpp` 仅做 PE 检测（只报不跑）。
- `winapp <file>` → 真正执行器：
  - 32 位 `win32_run`：`machine==0x014C && magic==0x010B`，否则返回 `-3`。
  - 64 位 `win64_run`：`MAXPE=192KiB` 截断。
  - `win32_ensure_init()` 必须在 PE 启动前调用。
- 浏览器：32 位 = `iexplore.exe`（`winpe/iexplore.c`，真实 PE32 i386）；64 位 = C# 托管 `BrowserApp`（默认 `https://www.bing.com/`）。验证地址栏编辑靠串口 `[browser] addr=` / `[iexplore] addr=` 长度序列，**不靠像素**。

---

## 7. 浏览器 IE：agent 桥（2026-08-09）

- 私有消息 `WM_MINIOS_API=0x8000`：内核 `webapi` 命令（口令鉴权）向 IE 投递（`verb=wParam`, `arg=lParam`）。
- agent verbs：`PING=1` / `GET_URL=2` / `NAVIGATE=3` / `GET_TEXT=4` / `ASK=5` / `CLICK=6` / `LINKS=7` / `STATUS=8`。
- AI 桥：iexplore 惰性绑定 `MiniAiReady` / `MiniAiInit` / `MiniAiAsk` → 内核 `kern_ai_*` → `ai_engine.cpp`（无需外部模型，`ai_init` 恒成功）。
- 口令：24 字节 XOR 混淆 `WEBAPI_PW`；`webapi auth <pw>` 解锁；≥3 次失败锁死。

---

## 8. 锁屏 + 默认 GUI + 取消 ESC（2026-08-10）

- 默认进图形：`kmain` / `kmain64` 在 `g_auto_gui && g_vbe_active` 时 `cmd_gui` 自动进桌面。`g_auto_gui` 默认 1，**勿被临时诊断改回 0**。
- ESC 不退终端：GUI 事件循环 `0x1B` 只交 `gui_handle_key`；`Login.Key` 遇 27 return。串口标记 `[GUI] ESC ignored`。
- 图形登录：`csharp/apps/Shell/Login.cs` 提交 `Host.LoginCheck`；内核 `gui_cb_login_check` 比对哈希（`[K32-LOGIN] OK user=root`）；哈希不出内核。默认账户 `root/admin`、`guest/guest`。
- 文本终端已被 GUI「Terminal」窗口取代（桌面/开始菜单图标）；切 64 位：登录后点 Terminal 图标输 `switch64`。

---

## 9. 中文位图字体（2026-08-11）

- 字库 `zfont_data.h`（GB2312 16×16，387 字）；`zfont_find_unicode()` 按 **Unicode 码点二分**。
- **历史根因**：`gen_zfont.py` 原按区位码排序，与运行时二分不一致 → 常用字命中错索引画方框。
- 修复：生成 `zfont_unicode[]` 按 **Unicode 严格升序**（`.bin` 位图块仍按 GB 顺序兼容旧逻辑）。**扩充字库必须保证该表升序**，否则重现方框 bug。

---

## 10. UEFI >4GB 帧缓冲黑屏修复（2026-08-11，VirtualBox 回归通过，待真机验证）

### 根因
华为 Iris Xe 的 GOP 帧缓冲落在 `>4GB`。`bootuefi.c` 原来分配一块 `<4GB` 的 shadow RAM，把 `framebuffer_phys` 指向 shadow 让内核画，但**没有任何代码把 shadow 拷回真实 GOP fb** → 黑屏（风扇转、CPU 在跑）。VirtualBox Bochs VBE 的 fb 在 `0xE0000000`（`<4GB`），不触发 shadow 分支，故虚拟机"一直正常"。

### 关键事实
- `enter_kernel.S` **没有退出 long mode**，而是进入 IA-32e 32 位兼容模式并沿用 UEFI 4 级页表 → 修复必须在**长模式内建 4 级页表**，不是传统 PAE 退出长模式。

### 修复（已落地）
1. `uefi/bootuefi.c`：移除无效 shadow；`framebuffer_phys64` 保留真实 `>4GB` 地址，`framebuffer_phys` 写低 32 位，`shadow_buffer=0`。
2. `kernel.cpp vmm_init()` 的 long-mode 分支：当 `fb64>4GB && shadow==0`，自建 4 级页表：低 4GB 用 2MiB 页身份映射 + 在 `0xF0000000` 建虚拟窗口映射真实高 fb；CR3 指向自建 PML4，CPU 仍留 long mode；随后改写 `VbeInfo.framebuffer_phys=0xF0000000`，`shadow_buffer=1`。
3. `gui.cpp`：更新注释，`framebuffer_phys` 现在始终指向可写帧缓冲（真实低 fb 或映射后的窗口 VA），`>4GB` 不再 abort。

### 验证
- VirtualBox 回归：正常进图形锁屏，关键标记 `[VMM] Firmware paging active (not our own)` 证明 `<4GB` 路径未触发新映射、行为未变。
- **待真机**：烧录 `build/os_uefi.img`，COM1 看是否出现 `[VMM] high FB 0x... mapped to VA 0xF0000000` 且屏幕不再黑屏。

---

## 11. 真机白条（白线）排查（截至 2026-08-11 状态：诊断构建已落地，待真机定性）

### 已排除的方向（经全量静态审计 + 修复）
- **pitch/stride 全链路**：`present()` 原忽略 LFB `pitch` 按 `width*bpp` 线性写，在 Intel GOP（`PixelsPerScanLine > width`）上把整图压进前几行 → **已改为四个像素格式分支均按 `lfb + ry*pitch` 逐行写**（`present_rect()` 本来就正确）。虚拟机 Bochs VBE `pitch==width*4` 掩盖此 bug。
- **像素格式**：`bootuefi.c` 已正确处理 `PixelBitMask`（标准 Intel 掩码 `0x00FF0000/0x0000FF00/0x000000FF`→BGRX32；`0xF800/0x07E0/0x001F`→RGB565；否则默认 BGRX32）。**格式不是根因**。
- **shadow 逻辑**：上一轮已移除无效 shadow（见 §10）。用户曾怀疑"shadow 覆盖真实地址让内核画在普通 RAM"，已确认不成立。

### 诊断构建 `make uefi-diag`（不污染正常 `uefi` 构建）
- 独立产物链：`gui_diag.o`（gui.cpp 加 `-DFB_DIAG`）、`bootuefi_diag.o`（加 `-DFB_DIAG`）、`kernel_diag.bin`、`BOOTX64.EFI`、`esp_diag.img`、`os_uefi_diag.img`。
- 诊断 blob 借用 `kernel.blob` 文件名以匹配 `get_embedded` 符号，链接后**立即把 `kernel.blob` 还原为正常内核**（已 `cmp` 校验与 `kernel.bin` 一致）。
- ESP 内 boot 文件必须叫 `BOOTX64.EFI`（初版误命名 `BOOTX64_DIAG.EFI` 导致无法自动引导，已修正）。

### 诊断内容
- `bootuefi.c`：新增 `[GOPF]` 串口行（FB_DIAG 守护），打印 GOP `PixelFormat` 枚举值、`PixelsPerScanLine`、`PixelBitMask` 的 R/G/B 掩码。
- `gui.cpp`：`gui_fb_diag()`（FB_DIAG 守护，在 `gui_enter()` 入口调用，画完 `hlt` 停机）**绕过 backbuffer 直接写 LFB**：
  - 默认（无 `FB_TEST`）：竖条彩虹条（一次看清 fb_base + pitch + 格式）。
  - `FB_TEST=1`：左上红方块（测试 fb_base 可写）。
  - `FB_TEST=2`：全屏蓝（测试帧缓冲范围 / pitch 健全性）。
  - `FB_TEST=3`：竖条但强制用 `width*4` 作 pitch（反向确认 pitch 是否为元凶）。
  - 串口 `[DIAG]` 打印 lfb / 宽 / 高 / pitch / w*4 / bpp / 格式。

### 判定标准（真机烧录后看屏幕 + COM1）
| 现象 | 结论 / 下一步 |
|---|---|
| 默认竖条均匀无撕裂 | fb_base + pitch + 格式全对，白条另有因（查双缓冲/翻转或显存未清零） |
| `FB_TEST=1` 红块不可见 | `>4GB` 高 fb 映射失败（fb_base 无效），查 `vmm_map_high_fb()` |
| `FB_TEST=2` 全屏蓝仍有横白条 | pitch 偏差残留 |
| `[GOPF]` 显示 `>4GB` + `PixelBitMask` 非标掩码 | 需扩 `bootuefi.c` 格式适配分支（当前 BitMask 只识别两种标准掩码） |

### QEMU 冒烟结果
`make uefi-diag` 成功（128MiB）；QEMU+OVMF 确认启动并打出 `ST`→`[GOP]`→`[GOPF] 1 00000000...`（QEMU std VGA 报 RGBX32、掩码 0）→`[4]`/`[5]`/`EGI3SFB`（32 位内核已进入），随后 QEMU 复位回 Shell（环境性不兼容，与诊断代码无关）。

---

## 12. 给接手 agent 的速查清单

1. **改完必验证**：编译通过不算完，要真引导/截图/串口。
2. **清旧产物**：WSL 内 `rm -f`，别用 Windows `rm`；怀疑没重编就先删再 `make`。
3. **GUI 改动优先 C#**，内核只留桥。
4. **UEFI 镜像 ESP 内的 boot 必须 `BOOTX64.EFI`**。
5. **真机显示问题**：VirtualBox/QEMU 的 Bochs VBE / std VGA 恒 `<4GB` 且 `pitch==width*4`，**所有真机专属 bug 都被掩盖**，最终定性必须靠真机 COM1 + 屏幕。
6. **现有可直接 load 的 skill**：`nexos-uefi-high-fb-fix`（位于 `~/.workbuddy/skills/nexos-uefi-high-fb-fix/`）——处理 Iris Xe `>4GB` 帧缓冲黑屏时优先调用。

---

## 13. 关键串口标记速查

| 标记 | 含义 |
|---|---|
| `ST` | bootuefi 启动 |
| `[GOP]` | 打印 FrameBufferBase |
| `[GOPF] <fmt> <ppsl> <rmask> <gmask> <bmask>` | GOP 真实像素格式/扫描线/掩码（诊断构建） |
| `EGI3SFB` | 32 位兼容模式切换完成（enter_kernel.S） |
| `[K1]` / `[K32]` | 32 位内核进入 / 进 GUI 模式 |
| `[DGOP] fb_base=.. fb64=.. w=.. h=.. bpp=.. pitch=.. nominal=.. MATCH/MISMATCH` | 内核帧缓冲参数 + pitch 一致性 |
| `[VMM] Firmware paging active (not our own)` | `<4GB` 路径，未触发自建映射 |
| `[VMM] high FB 0x... mapped to VA 0xF0000000` | `>4GB` 高 fb 映射成功（真机目标） |
| `[DIAG] ...` | 诊断构建 gui_fb_diag 帧缓冲参数 |
| `[CLR] MiniCLR initialised` | C# CLR 初始化完成 |
| `[LOGIN] lock screen armed` | 锁屏就位 |
| `NexOS.Forms.Shell initialised` | C# 托管壳上线 |
| `[GUI] Entered GUI mode` / `[GUI] ESC ignored` | GUI 进入 / ESC 被忽略 |
| `[browser] addr=` / `[iexplore] addr=` | 浏览器地址栏变化（验证用） |
| `[CLR] fault: too many string literals` | 字符串字面量超限，上调 `CLR_MAX_STRINGS` |
