# NexOS 路线图（现实版 v1.0）

> **本文件是项目宪法，取代以下两份文档，避免三份规划互相矛盾：**
>
> 1. 最初「NexOS + Wine 兼容层 完整项目规划」（从零模板，P4–P5 不可行）
> 2. `WIN32_NT_ROADMAP.md`（Win32 子系统渐进路线，已被本文件吸收）
>
> `NEXOS_AGENT_HANDOVER.md` 保留为"经验交接档案"，与本文件互补，不冲突。
>
> **核心校正**：真机白条是当前唯一未关闭的显示 bug；P4–P5 原案的"移植 Wine 跑 Chrome"  
> 不现实（Wine 200 万+ 行、Chrome 是 L4 数年工程），本路线图改用**扩展自研  
> PE/Win32 子系统**的务实路线——该子系统已能跑自研 IE 浏览器。



---

## 0. 项目现状（一句话）

NexOS 已是一个功能完整的 x86 教学操作系统：UEFI + BIOS 双引导、32/64 双内核、  
帧缓冲 GUI（C# 托管壳）、自制文件系统、网络栈、用户/权限系统、AI 引擎，以及一个  
**自研 PE/Win32 子系统**（已能加载并运行 `winpe/iexplore.exe` 自研浏览器）。  
当前唯一未关闭的显示问题是**真机（Intel Iris Xe）GUI 白条**。

---

## 1. 架构与构建铁律（来自实测经验，必须守）

| 铁律                   | 说明                                                                                                                  |
| -------------------- | ------------------------------------------------------------------------------------------------------------------- |
| **构建环境**             | 所有 Linux 构建一律用 **WSL**：`wsl -e bash -lc 'cd /mnt/d/MyOS/bootloader && make <target>'`                               |
| **Windows `rm` 被拦截** | 清旧产物在 WSL 内 `rm -f`，不要在 Windows 侧删                                                                                  |
| **时钟偏移跳过重编**         | WSL/Windows 时钟差会让 make 跳过重编译 → 改完先删旧产物再 `make`                                                                      |
| **验证纪律**             | 编译通过 ≠ 完成；用户要求必须实测（引导/截图/串口）才交付                                                                                     |
| **虚拟机掩盖真机 bug**      | VirtualBox/QEMU 的 Bochs VBE/std VGA 帧缓冲恒 `<4GB`、`pitch==width*4`，**掩盖**所有 `>4GB`/pitch 真机 bug；定性必须靠**真机 COM1 + 屏幕** |
| **GUI 优先改 C#**       | 桌面/窗口/菜单/图标等尽量只改 `csharp/`；C++ 内核层只留 host 回调与 P/invoke 桥                                                            |
| **品牌统一**             | NexOS（PascalCase）；新增 App 用 `NexOS.Forms`/`NexOS.Core`，勿用 MiniOS/miniOS                                              |
| **C# CLR 约束**        | 无浮点/泛型/接口/try-catch；数组 4B 槽；静态初始化器不执行（需 `Shell.Init()` 显式赋值）；堆每帧回卷                                                  |

### 主要 make 目标

```
make                      # 默认 = all = build/os.iso（CD/ISO，BIOS+UEFI 混合布局）
make bios-run / bios-test # 构建并运行 raw BIOS 镜像 build/os.img（含 kernel64 @LBA2048）
make hybrid               # build/os_hybrid.img（USB 盘镜像，内含 os.img）
make uefi                 # UEFI GPT 镜像 build/os_uefi.img
make uefi-diag            # 白条诊断镜像 build/os_uefi_diag.img（独立产物链，不污染 uefi）
make test / uefi-test     # 无头自动测试（串口 + 截图）
make play                 # C# GUI 看 QEMU 窗口
```

### 串口标记体系（调试命脉）

`[UEFI]/E/G/3/S`（bootloader）→`[K1]…[K6]`（kmain 阶段）→`[K32]`（进 GUI）  
→`[GOP]/[GOPF]`（GOP 参数/像素格式）→`[DGOP]`（内核帧缓冲 dump）→`[DIAG]`（诊断探针）  
→`[VMM] high FB ... mapped to VA 0xF0000000`（>4GB 映射成功标记）

---

## 2. 阶段状态总览

| 阶段 | 名称                  | 状态       | 关键证据                                                                    |
| -- | ------------------- | -------- | ----------------------------------------------------------------------- |
| P0 | 内核基础（UEFI+kmain+串口） | **已完成**  | `boot.asm`/`stage2.asm`/`uefi/bootuefi.c`/`kernel.cpp`；UEFI 32/64 双架构已通 |
| P1 | 内存管理与驱动             | **已完成**  | `vmm` 含 >4GB 4 级页表映射；ATA/PS2/PCI 驱动在                                    |
| P2 | 文件系统                | **已完成**  | MKFS（可写）+ SFS（只读）+ VFS + NTFS；数据盘持久化                                    |
| P3 | 帧缓冲与 GUI            | **基本完成** | `gui.cpp`(6938 行) + C# 托管壳；>4GB 修复已落地；**剩真机白条**                         |
| P4 | Windows 兼容层         | **重定向**  | 不移植 Wine；扩自研 `win32.cpp`+`winloader.cpp`（已跑自研 IE）                       |
| P5 | 浏览器/大型 GUI 应用       | **重定向**  | 路线：扩自研 IE 引擎；Chrome 列为 L4 远景，不在承诺范围                                     |
| P6 | 网络与调试               | **基本完成** | `net.cpp`(77KB) NE2000+HTTP+TCP；GDB stub 待补                             |
| P7 | 安全加固                | **部分完成** | `perm.cpp`+`shadow`+`sudo`+9 位权限已做；应用沙盒/签名未做                            |

---

## 3. 已完成阶段详述

### P0 — 内核基础（已完成）

- `boot.asm`(Stage1, 512B) → `stage2.asm`(开 A20/GDT/32 位 PM) → `entry.asm` → `kmain`。
- `uefi/bootuefi.c`(gnu-efi) + `enter_kernel.S`(长模式→32 位兼容模式) + `get_embedded.S`。
- `kernel.cpp`(7472 行) 终端/输入/ATA/shell/文件系统/用户权限。
- 串口 `0x3F8` 输出 `[K1]…[K32]` 标记体系成熟。

### P1 — 内存管理与驱动（已完成）

- PMM/VMM 已支持分页与 4MiB PSE 身份映射。
- **>4GB 帧缓冲修复（Iris Xe 黑屏）**：`enter_kernel.S` 不退出 long mode，沿用 UEFI 4 级页表；  
  内核 `vmm_init()` 在 long-mode 分支对 `framebuffer_phys64>4GB` 自建 PML4，把真实高 fb 映射到  
  `0xF0000000` 虚拟窗口，`bootuefi.c` 保留真实 64 位地址（`shadow_buffer=0`），不再覆盖为 shadow。
- ATA PIO、PS/2 键鼠、PCI 扫描均可用。

### P2 — 文件系统（已完成）

- MKFS（自制可写，支持独立数据盘持久化）、SFS（预构建只读）、VFS 抽象、NTFS 只读浏览。
- `shadow`/`permdb` 持久化在 MKFS 数据盘，跨重启保留。

### P3 — 帧缓冲与 GUI（基本完成，剩白条）

- GOP/UEFI 帧缓冲 + BGA fallback（任意 QEMU/VirtualBox 显卡可进 GUI）。
- `gui.cpp`(6938 行) Win11 Portal 风格桌面 + 窗口管理；**C# 托管壳** `csharp/apps/Shell/`  
  （`Desktop.cs`/`Browser.cs`/`Login.cs`/`Shell.cs`/`Popup.cs`/`AiAgent.cs` 等）实现桌面/窗口/菜单/托盘。
- 中文 GB2312 16×16 点阵字库（`zfont_data.h`，387 字，二分按 Unicode 升序）；拼音 IME（`ime_dict.h`）。
- **未关闭项 — 真机白条**：VirtualBox/QEMU 因 `pitch==width*4` 无法复现；已建 `uefi-diag` 诊断镜像  
  （`[GOPF]` 原始格式/`[DGOP]` 帧缓冲 dump/`[DIAG]` 写后读探针/竖条·红块·全屏蓝测试图），待真机烧录定性。

### P6 — 网络与调试（基本完成）

- `net.cpp`(77KB)：NE2000 驱动 + TCP/IP 栈 + HTTP 服务器（`netstart` 后浏览器可访问）。
- **待补**：远程 GDB stub（`gdb_stub.cpp`）。

### P7 — 安全加固（部分完成）

- 用户库（`shadow`，FNV-1a 哈希）、9 位 `rwxrwxrwx` 权限（`perm.cpp`/`permdb`）、`sudo` 提权已做。
- **待补**：应用级沙盒（VFS 隔离）、数字签名验证、Y/N 跨目录授权弹窗。

---

## 4. 当前卡点：真机白条（P3 收尾，阻塞中）

**已完成的诊断基建**（均 `FB_DIAG` 守护，不污染正常 `uefi` 构建）：

- `Makefile` `uefi-diag` 目标 → 独立产物链 `gui_diag.o`/`bootuefi_diag.o`/`kernel_diag.bin`/`BOOTX64.EFI`/`esp_diag.img`/`os_uefi_diag.img`，已 `cmp` 校验 `kernel.blob==kernel.bin`（正常构建未污染）。
- `bootuefi.c` 新增 `[GOPF]` 串口行：GOP `PixelFormat` 枚举、`PixelsPerScanLine`、`PixelBitMask` R/G/B 掩码。
- `gui.cpp` `gui_fb_diag()`：绕过 backbuffer 直接写 LFB 画**竖条彩虹条**（真实 pitch）；`FB_TEST=1` 红方块、`=2` 全屏蓝、`=3` 强制 `width*4` pitch；画完 `hlt` 停机；含 3 像素**写后读**探针（`[DIAG] probe(...) MATCH/MISMATCH`）。
- QEMU+OVMF 冒烟已确认 bootloader 能启动并打印 `[GOP]`→`[GOPF]`→`[4]`/`[5]`/`EGI3SFB`（32 位内核已进入）；QEMU 复位在 GUI 前（环境性不兼容，VirtualBox/Iris Xe 能完整启动）。

**待用户执行（真机定性）**：

1. 烧录 `build/os_uefi_diag.img` 到 U 盘，华为 Iris Xe 本 UEFI 启动，COM1 接串口线抓日志。
2. 看串口：
   - `[GOPF]` 显示 `>4GB` + `PixelBitMask` 且 R/G/B 掩码**非** `0x00FF0000/0x0000FF00/0x000000FF` 或 `0xF800/0x07E0/0x001F` → 需扩 `bootuefi.c` 格式适配分支。
   - `[DGOP]` 若 `MISMATCH(pitch>width)` → pitch 偏差残留（已修，确认修对）。
   - `[DIAG] probe(...) MISMATCH` → 帧缓冲映射不可写（>4GB 高映射失败）。
3. 看屏幕：竖条均匀 → fb_base/pitch/格式全对（白条另有因，查双缓冲/清屏）；`FB_TEST=1` 红块不可见 → 高映射失败；`FB_TEST=2` 蓝屏仍有横白条 → pitch 偏差。

**判定标准对照（五类原因）**：

| 原因           | 诊断信号                                                 | 下一步                                   |
| ------------ | ---------------------------------------------------- | ------------------------------------- |
| ① fb_base 无效 | `[DIAG] probe MISMATCH` / `FB_TEST=1` 红块不可见          | 查 `vmm_map_high_fb()` 是否建 PML4、CR3 时机 |
| ② 色彩格式不匹配    | `[GOPF]` 非标 `PixelBitMask`                           | 扩 `bootuefi.c` 格式分支                   |
| ③ 分辨率/尺寸不匹配  | `[DGOP]` 宽高/pitch 与显示不符                              | 核对 `present()` 步长                     |
| ④ 双缓冲/翻转未触发  | 竖条 OK 但真实 GUI 仍白线                                    | GOP fb 即前台缓冲，无需翻页；查增量 `present_rect`  |
| ⑤ 显存未清零      | 已排除（`force_clear_lfb` 用 `pitch*height` 覆盖，GUI 入口已调用） | —                                     |

---

## 5. Windows 兼容层路线（替代原 P4–P5 的 Wine+Chrome）

### 5.1 为什么不再"移植 Wine"

- 真实 Wine ≈ 200 万+ 行 C，依赖完整 POSIX/Unix syscall 面、X11/Win32 图形、fontconfig 等，  
  **不是 4 周能移植**的。
- 在从零 OS 上用 Wine 跑真实 Chrome：Chrome 依赖 GPU 合成、多进程沙箱、完整网络栈，  
  连 Wine 社区都难以稳定支持 → **L4，数年工程，不在 hobby 规模承诺范围**。
- **仓库已有更现实的资产**：`winloader.cpp`(PE 加载器) + `win32.cpp`(124KB 自研 Win32 子集)
  - `winpe/iexplore.c`(自研 IE 浏览器，已编译为 `iexplore.exe` 在本系统跑通)。  
    → 路线是**扩展这套自研层**，不是另起 Wine。

### 5.2 兼容能力尺度（L0–L4，来自实测调查）

| 级别         | 代表软件              | 需要的能力                      | 工作量参照                    |
| ---------- | ----------------- | -------------------------- | ------------------------ |
| **L0**（现状） | hello32、画矩形 GUI   | 已有                         | 已完成                      |
| **L1**     | 文件读写 + 多线程小工具     | 真堆/VM、线程、文件 IO、CRT         | 数周                       |
| **L2**     | 老式 32 位 GUI（记事本类） | 完整 user32/gdi32、DC、控件、消息   | 数月                       |
| **L3**     | 需 COM/网络的程序       | ole32、winsock、advapi、shell | 半年~1 年                   |
| **L4**     | **Chrome/现代浏览器**  | 多进程+IPC+沙箱+GPU+全套 API      | **数年 / Wine·ReactOS 级别** |

### 5.3 渐进里程碑（每个阶段都有可运行演示）

- **阶段 1 — 加载器硬化（x64 + 真 DLL 模型）【L0→L1】**
  - 支持 PE32+（x64）：`machine==0x8664`、`magic==0x20B`、REL64 重定位、8 字节 IAT。
  - 真 `LoadLibraryA`/`GetProcAddress`：查兼容层导出表，未命中返回 NULL（不再假基址）。
  - 依赖 DLL 导入解析（kernel32/user32/gdi32 伪 DLL 导出库）、TLS、延迟加载。
  - 取消 8MB `SizeOfImage` 硬上限，改按可用内存动态分配。
  - **演示**：加载"空导入表自包含 32/64 位 PE"并调用入口，打印返回值。
- **阶段 2 — Kernel32/CRT 实质化【L1】**
  - 真堆 `HeapCreate/Alloc/Free`（多堆替 bump）；`VirtualAlloc/VirtualFree/VirtualProtect`。
  - 线程 `CreateThread`/关键段/`Event/Mutex/Semaphore`/`WaitForSingleObject`。
  - 文件 IO `CreateFile/ReadFile/WriteFile/FindFirstFile`（接 NexOS VFS）。
  - 时间 `GetTickCount/QPC/Sleep`；控制台 `WriteConsole/ReadConsole`；完整 `msvcrt`。
  - **演示**：跑一个**真做文件读写 + 开线程**的 MinGW 32 位 Win32 小程序。
- **阶段 3 — User32/GDI 实质化【L1→L2】**
  - 多窗口、z-order、子窗口、控件（BUTTON/EDIT/STATIC 真实行为）。
  - 消息循环：定时器/键盘/鼠标（NexOS 输入 → Win32 消息）。
  - 真 `HDC`：设备上下文、位图 `CreateCompatibleDC/BitBlt/DIB`、画刷/画笔、字体度量。
  - GDI 后端直接画到 NexOS 帧缓冲（复用 `gfx` 原语），淘汰显示列表桥。
  - **演示**：跑带按钮/文本框/能打字的 32 位 GUI 程序（记事本类）。
- **阶段 4 — 网络与系统库【L2→L3】**
  - Winsock 接 `net.cpp` TCP；`advapi32` 注册表/令牌；`shell32`/`wininet` HTTP 客户端。
  - **演示**：能联网下载网页的 32 位程序（自研小浏览器雏形）。
- **阶段 5 — COM 与现代底座【L3】**
  - `ole32/oleaut32`：`CoInitialize/CoCreateInstance`、类厂、引用计数、`QueryInterface`。
  - **演示**：依赖 COM 的 32 位程序。
- **阶段 6 — 多进程+沙箱+GPU（Chrome 专属）【L4，长期/远景】**
  - 真 `CreateProcess`（独立映像+堆+IPC）；Chrome 沙箱令牌；GPU 软件光栅或帧缓冲后端。
  - **仅在阶段 1–5 扎实后才可能；Chrome 不承诺跑通。**

### 5.4 浏览器路线（P5 的现实替代）

- **近期**：扩展自研 `winpe/iexplore.c` 引擎（已能在 NexOS 跑），补 GDI/网络，做更完整的浏览器体验。
- **中期**：阶段 4 后可用自研小浏览器雏形联网。
- **Chrome**：列为 L4 远景，不作为交付承诺；若未来要碰，必须先完成阶段 1–5 且补齐 x64 用户态。

### 5.5 第一个真动手里程碑（建议起点）

> 阶段 1 + 阶段 2 精简版：让 NexOS 能运行一个"**做文件读写 + 开线程**"的真实 32 位  
> Win32 程序（MinGW 交叉编译 `hello-with-fileio.exe`），并跑通。  
> 工作量可控（数周），立刻证明兼容层从"玩具"变"真能跑 Windows 程序"，  
> 无需先碰 x64/COM/GPU 无底洞。

---

## 6. 剩余 P6/P7 收尾清单

- **P6 待补**：远程 GDB stub（`gdb_stub.cpp`，串口断点调试内核）。
- **P7 待补**：应用级沙盒（VFS 隔离，每 App 只访问自己目录）、数字签名验证  
  （未签名 App 触发告警）、Y/N 跨目录授权弹窗。

---

## 7. 执行约束（给接手 Agent）

| 约束   | 说明                                                                               |
| ---- | -------------------------------------------------------------------------------- |
| 代码风格 | C++17 freestanding（无 libc）；C# 走 `NexOS.Core`/`NexOS.Forms`                       |
| 目标平台 | x86_64 UEFI + 32 位兼容模式（内核 32 位，`kernel.bin`@0x10000；64 位 `kernel64.bin`@LBA2048） |
| 调试接口 | 串口 `0x3F8`，`[标记]` 格式日志                                                           |
| 真机测试 | 华为笔记本（11 代 i5，Iris Xe）— **所有显示 bug 定性必须真机 COM1 + 屏**                             |
| 禁止   | 内核 C++ 浮点/异常/RTTI；C# 浮点/泛型/接口/try-catch                                          |
| 验证   | 每阶段产出可运行演示 + 截图/录屏/串口日志，避免"写几年跑不出东西"                                             |

---

## 8. 验收标准（分阶段）

| 阶段 | 验收测试                             | 命令/方式                       | 状态              |
| -- | -------------------------------- | --------------------------- | --------------- |
| P0 | 串口打印 `[K1]`                      | 真机/VM 启动                    | ✅ 通过            |
| P1 | 分配 1MB 物理内存 + 读 ATA              | `meminfo`                   | ✅ 通过            |
| P2 | 创建文件并持久化                         | `write test.txt` + 重启 `cat` | ✅ 通过            |
| P3 | GUI 桌面正常显示（无白条）                  | `gui` / 真机屏                 | ⚠️ 待真机白条定性      |
| P4 | 跑真实 32 位 Win32 程序（fileio+thread） | `run hello-with-fileio.exe` | ⬜ 阶段 1+2 待做     |
| P5 | 浏览器主窗口显示                         | 扩自研 IE / 小浏览器               | ⬜ 渐进            |
| P6 | HTTP 服务器返回 HTML + GDB 断点         | `netstart` + GDB stub       | 🟡 HTTP✅ / GDB⬜ |
| P7 | 沙盒阻止跨目录访问                        | `run evil.exe` 弹窗           | 🟡 权限✅ / 沙盒⬜    |

---

## 9. 交付物与风险

**交付物**：内核源码（C+++asm）、C# 托管壳、自研 Win32 子系统、构建镜像  
（`os.img`/`os.iso`/`os_uefi.img`/`os_uefi_diag.img`）、真机启动镜像、各阶段演示与日志。

**主要风险**：

1. **真机白条未定性**（最高优先）：依赖用户烧录诊断镜像 + COM1 + 屏，Agent 侧无法在 VM 复现。
2. **Chrome 不在承诺范围**：阶段 6 仅给方向，避免排期误导。
3. **x64 是硬前置**：不补 PE32+ 与 64 位用户态，连 Chrome 入口都进不去（影响远期）。
4. **每阶段独立交付演示**：即使未到 L4，L1/L2/L3 兼容层对"在 NexOS 跑旧 Windows 软件"已有实用价值。

---

**启动原则**：先关真机白条（P3 收尾），再按 5.5 启动 Win32 兼容层第一个里程碑；  
Chrome 列为远景，不占用近期排期。
