# NexOS — 自研 x86 操作系统（内核 · C# 桌面壳 · Web 控制台 · AI · 分布式算力）

> ⚠️ **协作规则：见 [RULES.md](RULES.md)。**
> 任何更改（含 AI 助手完成的）都**必须提交 git 并推送 GitHub**（remote `origin`，分支 `master`）。
> 改动大文件前请先备份并做锚点校验——本项目曾因未提交的工作被误删而无法恢复。

<div align="center">

[![中文](https://img.shields.io/badge/中文-简体-red)](#中文版本)
[![English](https://img.shields.io/badge/English-README-blue)](#english-version)
[![Roadmap](https://img.shields.io/badge/架构-NEXOS_ROADMAP-blue)](NEXOS_ROADMAP.md)

</div>

## <a id="中文版本"></a> 🇨🇳 中文版本

## 项目定位

NexOS 是一个从零自研的 **x86 操作系统**：起点是一个支持 BIOS + UEFI 双引导、可切换 32/64 位保护模式的 C++ 内核与二阶引导加载器，如今已演进为**融合图形桌面、本地大模型推理、分布式算力网络与 Windows/Linux 兼容能力的全栈系统**：

- **内核与引导**：Stage1→Stage2 二阶引导，32 位保护模式内核 `kernel.cpp` + 64 位长模式内核 `kernel64.cpp`，UEFI/BIOS 双路径汇合到同一 C++ 入口。
- **图形桌面（GUI）**：`gui.cpp` 在帧缓冲上自绘 Win11 风格桌面；其上叠加 **C# 托管壳**（`csharp/NexOS.Forms` / `NexOS.Core`，`csharp/apps/Shell` 以 `Desktop.cs`/`Browser.cs`/`Login.cs`/`Shell.cs`/`AiAgent.cs` 实现窗口管理器、菜单、托盘、登录与 AI Agent）。
- **Web 控制台**：`win11-ui/` 提供浏览器端 Win11 桌面 + 分布式网络面板 + Visual Agent Forge，经 WebSocket 桥（`tools/nexos_bridge.py`）通过串口直连**真 QEMU 虚拟机内核**，登录/命令/算力任务全部真实执行（无本地模拟）。
- **AI 能力**：Markov + GPT 风格文本引擎（`ai_engine.cpp`）+ 内置 **Qwen2-0.5B (Q4_K_M) GGUF Transformer 推理引擎**（`gguf_infer.cpp`，`ask64` 切换 64 位流式推理）；`agent/` 智能体与 Agent Forge 可视化编排。
- **分布式算力网络**：内核内 `distnet.cpp`（UDP 广播发现 + 任务调度 + 结果回收）+ 独立运行时 `dcn/`（crypto/discovery/sched/transport/wifi），可将推理/计算任务**分片下发到多 VM / 宿主节点合并推理**。
- **兼容层**：自研 **PE/Win32 子系统**（`winloader.cpp` + `win32.cpp`，已能跑自研 IE 浏览器）；**Linux 二进制兼容**（`linux_compat.cpp` + `linux_root/`，运行 ELF/动态链接程序）；**Mono 运行时移植**（`mono_port/`，在 NexOS 上跑 .NET 托管代码）。
- **系统设施**：双文件系统 MKFS（可写、独立数据盘持久化）+ SFS（只读）、用户/权限/sudo、网络栈（NE2000 + HTTP）、中文点阵字库 + 拼音 IME。

> **一句话定位**：以自研 C++ 内核为底座，融合 C# 托管桌面壳、浏览器 Web 控制台、本地大模型推理与分布式算力网络的**教学级全栈操作系统**。

## 核心特性

- **引导与内核**：BIOS + UEFI 双引导；32 位 `kernel.cpp` 与 64 位 `kernel64.cpp`（长模式）双内核；`switch64`/`switch32` 互切。
- **终端与 shell**：滚动回看终端 + 剪贴板（鼠标选择/中键粘贴/Ctrl+C/V）；PS/2 键鼠驱动；PowerShell 风格 shell，48+ 命令。
- **用户与权限**：登录/多用户/密码哈希（影子文件持久化）；9 位 `rwxrwxrwx` 权限；`sudo` 提权。
- **文件系统**：MKFS（自制可写，独立数据盘持久化）+ SFS（只读）+ VFS；NTFS 只读浏览；`.sh` 脚本执行。
- **GUI 桌面**：`gui.cpp` 帧缓冲自绘 Win11 风格桌面 + **C# 托管壳**（`NexOS.Forms`/`NexOS.Core`）窗口管理器；按需启动，BGA fallback 兼容任意显卡。
- **中文与 IME**：GB2312 16×16 点阵字库（387 字）+ 拼音输入法（GUI Terminal 内拼音→选字→UTF-8）。
- **Web 控制台**：`win11-ui/` 浏览器 Win11 桌面 + 分布式网络面板 + Agent Forge，经 WebSocket 桥连真内核。
- **AI 推理**：`ai_engine.cpp`（Markov+GPT）文本生成；`gguf_infer.cpp` 内置 Qwen2-0.5B GGUF Transformer，`ask64` 流式推理；`agent/` 智能体。
- **分布式算力**：`distnet.cpp` UDP 发现/调度 + `dcn/` 运行时（crypto/sched/transport），任务分片到多节点合并推理。
- **兼容层**：自研 PE/Win32 子系统（`winloader.cpp`+`win32.cpp`，可跑自研 IE）；Linux 二进制兼容（`linux_compat.cpp`+`linux_root/`）；Mono 运行时移植（`mono_port/`）。
- **网络**：NE2000 驱动 + TCP/IP + HTTP 服务器（`netstart` 后浏览器可访问）。

## 顶层目录与子模块

| 目录 / 文件                | 角色                                                          |
|----------------------------|---------------------------------------------------------------|
| `boot.asm` `stage2.asm` `entry.asm` `entry64.asm` | Stage1/Stage2 二阶引导、32/64 位内核入口桩          |
| `kernel.cpp` `kernel64.cpp` `switch32to64.asm` `switch64to32.asm` `smp_impl64.cpp` | 32/64 位内核与模式切换、SMP        |
| `gui.cpp`                  | 帧缓冲 GUI 桌面 + 中文渲染 + 拼音 IME                          |
| `winloader.cpp` `win32.cpp` | 自研 PE/Win32 子系统（加载并运行 Windows exe）               |
| `net.cpp`                  | NE2000 网卡驱动 + TCP/IP + HTTP 服务器                         |
| `ai_engine.cpp` `gguf_infer.cpp` `gguf*.{c,h}` `ai_*` | AI 文本生成 + Qwen2 GGUF 64 位推理引擎           |
| `distnet.cpp` `distnet.h`  | 内核内分布式算力网络：UDP 发现 + 任务调度 + 计算节点/Agent      |
| `linux_compat.cpp` `linux_compat.h` `linux_root/` | Linux 二进制（ELF/动态链接）兼容层                  |
| `uefi/bootuefi.c` `uefi/enter_kernel.S` `linker.ld` `linker64.ld` | UEFI 引导与 32/64 位链接脚本              |
| `csharp/`                  | **C# 托管壳**：`NexOS.Core`/`NexOS.Forms` API + `apps/Shell`（桌面/窗口/浏览器/登录/AI Agent）+ `winhost` 宿主 |
| `win11-ui/`               | **Web 控制台**：浏览器 Win11 桌面 + 分布式网络面板 + Agent Forge（`nexos-desktop.html`）+ WinUI 3 标准控件/令牌 |
| `dcn/`                    | **分布式算力运行时**：`dcn_*`(crypto/discovery/sched/transport/wifi/kernel) + `rt/`(运行时) + `build_qemu/` |
| `mono_port/`              | **Mono 运行时移植**：`pal/`(平台抽象层) + 构建产物，在 NexOS 上跑 .NET 托管代码 |
| `agent/`                  | AI 智能体实现（与 `ai_engine`/`gguf_infer`/`distnet` 协作）     |
| `tools/`                  | 构建与运维脚本：`sfs_gen.py`、`embed_model.py`、`gen_zfont.py`、`gen_ime_dict.py`、`nexos_bridge.py`、`nexos_l2hub.py`、`distnet_host_peer.py`、`check_k64_fit.sh`、`analyze_login.py`/`analyze_ppms.py` 等 |
| `sfs_files/` `docs/` `Makefile` `test.sh` `test_uefi.sh` | SFS 源文件、规划文档、构建与无头测试         |

## 内存与磁盘布局

```
内存:  0x07C00  Stage1 (BIOS 加载)
       0x08000  Stage2 (Stage1 加载)
       0x10000  32 位 C++ 内核 (Stage2 / UEFI bootloader 加载)
       0x100000 64 位内核 (switch64 加载, 长模式)
       0x90000  32 位栈顶

BIOS 磁盘 (LBA, 扇区=512B):
       LBA 0         boot.bin     (Stage1, 512B)
       LBA 1..32     stage2.bin   (Stage2, 固定 16KiB)
       LBA 33..544   kernel.bin   (内核, 最多 256KiB)
       LBA 300..303  命令历史文件  (save/load, 2KiB)
       LBA 2048..     kernel64.bin (64 位内核)
       LBA 16383     GGUF 描述符 (魔数 "MINIMDL1": size + data_lba)
       LBA 16384..    GGUF 模型数据 (如 Qwen2-0.5B Q4_K_M, ~397MB)
       LBA 512       MKFS 超级块   (魔数 "MKFS")
       LBA 513..528  MKFS 文件表   (16 扇区, 256 条目)
       LBA 529..799  MKFS 数据区   (启动盘: 271 扇区 / 135KB)
       LBA 800       SFS 超级块   (魔数 "SFS", Makefile 预生成)
       LBA 801..816  SFS 目录     (16 扇区, 256 条目)
       LBA 817..1023 SFS 数据区   (207 扇区, 103KB)

用户数据盘 (第二块 ATA 硬盘 / data.vhd, 8MB):
       LBA 512       MKFS 超级块   (魔数 "MKFS")
       LBA 513..528  MKFS 文件表   (16 扇区, 256 条目)
       LBA 529..     MKFS 数据区   (15200 扇区 ≈ 7.5MB)
```

Stage2 固定 16KiB,内核起始 LBA(33)确定。**MKFS 数据区默认写在独立的
用户数据盘**(Secondary/Primary 上第一个 ATA 硬盘),启动介质(ISO/CD)可
保持只读;没有数据盘时回退到启动盘(兼容旧镜像)。用户文件(含 `shadow`
用户库与 `permdb` 权限表)因此**跨重启持久化**。

UEFI 路径下,`BOOTX64.EFI` 从 ESP 读取 `kernel.bin` 复制到 `0x10000`,
退出 Boot Services 后切到 32 位兼容模式跳转——与 BIOS 路径汇合。

### 镜像分区与容量告警(改内核前必读)

`build/os_v2.img` 的实际分区(由 Makefile 变量决定):

| 区域 | LBA | 说明 |
|------|-----|------|
| boot + stage2 + kernel.bin | 0 .. ~1200 | 32 位部分 |
| `kernel64.bin` | 2048 .. 2048+`KERNEL64_SECTORS` | 64 位内核;32 位 loader 只搬 `KERNEL64_SECTORS` 个扇区 |
| 主 SFS | `SFS_LBA`(3568) | 只读文件系统 |
| Linux 用户态 FS | `LINUX_SFS_LBA`(3836) | 独立分区 |
| GGUF 描述符 / 数据 | 16383 / 16384 | 仅指定 `MODEL_GGUF` 时写入 |

**64 位内核的容量非常紧张**:它必须落在 LBA 2048 与 SFS 之间的“缝隙”里。
一旦内核涨出这个缝隙,`dd` 会**静默覆盖文件系统**,或把截断的镜像交给
`switch_to_64bit()` 导致 triple fault —— 属于“镜像长到硬编码偏移里”的经典故障。
Makefile 的 `$(IMG)` 规则有守卫会直接报错,但**改内核前先自检更快**:

```bash
bash tools/check_k64_fit.sh
```

当前状态:`KERNEL64_SECTORS=1500`、`SFS_LBA=3568`,kernel64 约 765KB,
**仅剩个位数扇区(约 2~3KB)余量**。继续往 64 位内核加功能时,必须同步:

1. 调大 `kernel.cpp` 的 `KERNEL64_SECTORS`
2. 把 `Makefile` 的 `SFS_LBA` 与 `kernel.cpp` 的 `SFS_ALT_LBA` **一起后移**(两者必须一致)
3. 相应后移 `LINUX_SFS_LBA`

> ⚠️ **已知既有问题**:`LINUX_SFS_LBA(3836)` 落在主 SFS 区间(3568..13390)
> **内部**,写 Linux FS 时会覆盖主 SFS 的一部分。目前没被触发是因为被覆盖区域
> 尚无实际文件数据;在扩展 SFS 之前,应先把 `LINUX_SFS_LBA` 移到主 SFS 之后。

> 💡 **WSL 构建提示**:WSL2 的 DrvFs 偶发“大文件写不进去 / 构建产物消失”。
> 若遇到,把产物目录指到 WSL 本地盘再拷回即可:
> `make BUILD=/home/<user>/nb && cp ~/nb/os_v2.img build/os_v2.img`

## 依赖

在 **WSL / 原生 Linux** 下，`make` 会**自动探测并 apt-get 安装**缺失的依赖
(首次构建时，会提示 sudo 密码):

| 目标 | 需要的包(自动安装) |
|------|--------------------|
| 基础(`make`) | `nasm g++ binutils python3 make` |
| 32 位内核(`-m32`) | `gcc-multilib`(或 `g++-multilib`) |
| UEFI(`make uefi`) | `gnu-efi ovmf` |
| 镜像/ISO(`make iso`/`uefi`) | `mtools xorriso` |
| 运行(`make run`) | `qemu-system-x86` |
| Windows PE(`make winpe`) | `gcc-mingw-w64-x86-64 gcc-mingw-w64-i686` |
| C# 应用(`make csharp`) | `dotnet-sdk-8.0` |

手动预装(可选):

```bash
# BIOS + 32 位内核
sudo apt install nasm g++ gcc-multilib binutils make python3 qemu-system-x86
# UEFI
sudo apt install gnu-efi ovmf mtools xorriso
```

> **WSL 说明**: 从 WSL 终端(`cd /mnt/d/MyOS/bootloader`)直接 `make` 即可,
> 全程使用 WSL 内原生工具链(nasm/g++/ld/objcopy/qemu/...),不再依赖
> Windows 侧的跨编译工具(如 `/d/nexos-tc/...`)。UEFI 固件用系统 OVMF,
> QEMU 用原生 `qemu-system-x86_64`(WSLg 下自动走 X11 后端)。
> 若不希望自动探测 WSL,可用 `make WSL=0 ...` 强制按原生 Linux 行为。

### 中文字库 / 拼音字典(可选,构建时自动)

- `gen_zfont.py` 需要 Pillow + 一个中文字体(Windows SimSun / 文泉驿等)
- `gen_ime_dict.py` 需要 pypinyin(`pip install pypinyin`)

## 构建

```bash
make                 # 生成 BIOS 镜像 build/os_v2.img (含 SFS)
make iso             # 生成 CD-ROM 镜像 build/os.iso (BIOS+UEFI 混合)
make uefi            # 生成 UEFI 镜像 build/os_uefi.img
make sfs             # 仅生成 SFS 镜像 build/sfs.img
make data-vhd        # 生成用户数据盘 build/data.vhd (8MB, 预格式化 MKFS)
make disasm          # 查看内核入口与反汇编,确认 _start 在 0x10000
```

### 嵌入 GGUF 模型(64 位 Qwen2 推理)

默认镜像不含模型权重。`make` 时传 `MODEL_GGUF` 会把 GGUF 写入镜像
(LBA 16383 描述符 + LBA 16384 数据,镜像自动扩容到 ~400MB+):

```bash
# 下载/准备 Qwen2-0.5B Q4_K_M GGUF(约 397MB)到 build/
make MODEL_GGUF=build/qwen2-0_5b-instruct-q4_k_m.gguf
# 或事后单独嵌入到任意已存在镜像:
python3 tools/embed_model.py build/os_v2.img build/qwen2-0_5b-instruct-q4_k_m.gguf 16383 16384
```

> 注意:`build/os.img` 默认是 auto-GUI 构建;验证 GGUF 推理请用
> `build/os_v2.img`(默认 IMG),并通过 QEMU `-device loader,addr=0x501E`
> 进入 headless 文本 shell(见下)。

SFS 镜像由 `tools/sfs_gen.py` 从 `sfs_files/` 打包;中文与拼音字典由
`gen_zfont.py` / `gen_ime_dict.py` 在 `gui.cpp` 编译前自动生成到项目根
(`zfont_data.h` / `ime_dict.h`)。

## 运行 / 测试

### BIOS

```bash
make test             # 无头自动测试:shell + 翻页 + save/load + 文件系统 + 脚本
make iso-run         # ISO 镜像 (os.iso)
```

### ISO + 数据盘(推荐,文件持久化)

```bash
make iso-run-data     # -cdrom os.iso + -drive data.vhd (IDE secondary)
```

### UEFI

```bash
make uefi-run         # QEMU 窗口(OVMF 固件)
make uefi-test        # 无头测试:串口 + 截图校验
```

`make test` 通过 monitor `sendkey` 模拟输入,分 6 阶段导出 `0xB8000`
校验:help/echo 回显、方向键翻页、save/load 历史持久化、MKFS 增删改查、
SFS 列目录/读文件/跑脚本。当前还含登录步骤(`root`/`admin`)。

`make uefi-test` 捕获串口验证:UEFI bootloader 加载内核 → 退出 Boot
Services → x64→32 位兼容模式 → kmain → VGA 文本模式 → Hello world。
OVMF 退出后 monitor 读 VGA 返回 `0xFFFFFFFF`,改用 `screendump` 截图
统计非黑像素验证屏幕有内容。

## 命令行 shell

启动后进入**登录提示**,输入用户名/密码(预置 `root`/`admin`、
`guest`/`guest`),成功后进入 shell,提示符 `PS user@minios /path>`。

### 基础命令

| 命令            | 作用                                     |
|-----------------|------------------------------------------|
| `help`          | 列出所有命令                             |
| `echo <text>`   | 打印文本                                 |
| `clear` / `cls` | 清屏并清空回看历史                       |
| `about`         | 系统信息                                 |
| `history` / `h` | 显示本次(及从磁盘载入的)命令历史         |
| `save`          | 把命令历史写入磁盘(LBA 300)              |
| `load`          | 从磁盘读回命令历史                       |

### MKFS 命令(自制可写文件系统)

| 命令            | 作用                                   |
|-----------------|----------------------------------------|
| `mkfs`          | 格式化 MKFS(数据盘或启动盘)            |
| `ls` / `dir`    | 列出 MKFS 上的文件                     |
| `cat` / `type`  | 打印 MKFS 文件内容                     |
| `touch` / `ni`  | 创建空文件                             |
| `write <f>`     | 逐行写入文本(空行结束,最大 8KB)       |
| `rm` / `del`    | 删除 MKFS 文件                         |
| `copy` / `cp`   | 复制文件                               |
| `mkdir` / `md`  | 创建目录                               |
| `cd` / `sl`     | 切换目录                               |
| `pwd` / `gl`    | 显示当前目录                           |

### SFS / 分区 / 脚本

| 命令            | 作用                                   |
|-----------------|----------------------------------------|
| `lsfs`          | 列出 SFS 上的文件                      |
| `catfs <f>`     | 打印 SFS 文件内容                      |
| `run <f>`       | 执行 MKFS 上的 `.sh` 脚本              |
| `runfs <f>`     | 执行 SFS 上的 `.sh` 脚本               |
| `part` / `mount` / `lsfat` / `fatinfo` | FAT32 分区管理与浏览 |

### 用户 / 权限 / sudo 命令

| 命令            | 作用                                          |
|-----------------|-----------------------------------------------|
| `whoami` / `id` | 显示当前用户 / uid / gid                       |
| `users`         | 列出所有用户                                   |
| `login <user>`  | 切换用户(输密码)                              |
| `logout`        | 退出登录,回到登录提示                         |
| `su <user>`     | 切换用户(默认 root,输密码)                   |
| `useradd <name> [pw]` | 添加用户(root only,默认密码 123456)      |
| `deluser <name>`| 删除用户(root only,不能删 root/当前用户)     |
| `passwd [user]` | 改密码(root 可改任何人;自己改需输旧密码)      |
| `chmod <mode> <file>` | 改文件权限(如 600/644/755,owner 或 root) |
| `stat <file>`   | 显示 owner / gid / mode(rwxrwxrwx + 八进制)   |
| `sudo <cmd>`    | 输密码提权后以 root 执行单条命令               |

### 内核 / GUI / 网络 / AI / 电源

| 命令            | 作用                                          |
|-----------------|-----------------------------------------------|
| `gui`           | 进入 GUI 桌面(BGA fallback,任意显卡可用)     |
| `switch` / `switch64` | 切换到 64 位内核(长模式)               |
| `netinfo` / `netstart` | 网络状态 / 启动 NE2000 + HTTP 服务器    |
| `ai` / `generate` / `agent` | AI 文本生成(初始模型 / 生成 / Agent) |
| `ask64 <q>`    | 切 64 位内核用内嵌 GGUF 做真实 Qwen2 推理(流式输出) |
| `ps` / `kill <pid>` | 进程列表 / 结束进程                                    |
| `meminfo` / `memtest` / `pagetest` | 内存 / 页表诊断            |
| `run xxx.bat/.exe/.ps1` | 通过 winloader 在 GUI 中打开并执行       |
| `shutdown` / `reboot` | 关机 / 重启                               |

## AI 推理(GGUF / Qwen2)

系统有两层 AI:

1. **32 位内置引擎**(`ai_engine.cpp`):Markov + GPT 风格文本生成,不需要
   模型文件,`ai` / `generate` / `agent` 命令直接可用(见 `test_ai.sh`)。
2. **64 位 GGUF 推理引擎**(`gguf_infer.cpp` + `kernel64.cpp`):从镜像嵌入的
   GGUF 加载真实 Transformer(Qwen2-0.5B Q4_K_M),做高质量生成。

### 触发真实 GGUF 推理

64 位内核的 `ask` 命令在 `qwen_ready()` 时才跑真实权重;而 64 位文本
shell 本身不接收 PS/2 键盘输入,所以推理由 **32 位 `ask64` 命令**触发:

```
ask64 <问题>
  └─ 32 位内核把问题写入共享内存 0x5104,魔法字 NEXQ 写入 0x5100
  └─ do_switch64() 切换到 64 位内核 (LBA 2048 的 kernel64.bin)
  └─ jump_to_64bit_and_infer() 检测到 NEXQ → 加载内嵌 GGUF → qwen_generate()
  └─ 流式输出到串口 / 终端,完成后返回 32 位 shell
```

### Headless 启动(windows 端 QEMU,TCG)

64 位内核在检测到 hypervisor(VM)时默认进文本 shell(不进 GUI)。用
`-device loader,addr=0x501E,data=1,data-len=1` 让 32 位内核停在文本
shell(不自动切换、不进 GUI),登录后即可输入 `ask64`。示例
(`build/run_ai_qemu.py` 已封装 monitor/Serial TCP 注入):

```bash
qemu-system-x86_64 -m 256 \
  -drive file=build/os_v2.img,format=raw,if=ide -boot c \
  -display none -monitor tcp:127.0.0.1:4460,server,nowait \
  -serial tcp:127.0.0.1:4461,server,nowait \
  -accel tcg -smp 4 \
  -device loader,addr=0x501E,data=1,data-len=1
```

登录 `root` / `admin` 后,通过 monitor `sendkey` 注入 `ask64 What is 2+2?`,
即可看到 64 位内核加载 GGUF 并推理。无模型时内核打印
`[AUTOTEST] no model blob found.`(管线本身已打通)。

### 已知修复 / 注意

- **64 位 SMP 在 QEMU/TCG 下卡死**:原 `smp_impl64.cpp` 的 `smp_init()`
  会对 APIC 1..3 广播 INIT/SIPI,而 QEMU TCG xAPIC 返回 Send Accept Error
  且 IPI 投递位常驻,导致 `smp_lapic_ipi()` 每次跑满超时、整体数分钟卡住。
  已在 `smp_init()` 开头加 **VM 检测**(cpuid leaf 1 bit 31):检测到
  hypervisor 时直接 `g_ncpus=1` 单核返回,GPUF 推理路径完全 BSP 本地,
  无需 AP。重编 `kernel64.bin` 后该问题消失。
- **headless 标志语义**:`0x501E` 原会让 32 位内核登录后自动 `cmd_switch64()`
  切到 64 位,导致没机会输入 `ask64`。已改为停在 32 位文本 shell,由用户
  显式 `ask64` / `switch64` 驱动 64 位推理(符合标志"文本 shell"本意)。
- 模型文件较大(~397MB),镜像需相应扩容;`make MODEL_GGUF=...` 会自动处理。
- **未嵌模型时的表现**:内核打印 `[AUTOTEST] no model blob found.`,管线本身
  已打通(详见下方「Web 控制台」里 Agent Forge 对思考节点的说明)。

## 分布式算力网络(distnet)

`distnet.cpp` 实现了一个真实的极简分布式计算网络:调度器用 **UDP 广播**发现
计算节点,把任务下发给节点执行并回收结果。

| 命令 | 作用 |
|------|------|
| `distnet nodes [ip]` | 发现计算节点(不带 IP = 广播;带 IP = 单播) |
| `distnet discover` | 广播发现(需要节点与调度器在同一 L2 广播域) |
| `distnet scheduler <type> <n>` | 广播下发任务并回收结果 |
| `distnet scheduler <ip> <type> <n>` | 单播下发到指定节点 |
| `distnet compute` | 本机作为计算节点上线(若干次 BEACON 后 idle-exit) |
| `distnet ai "<prompt>"` | 把推理任务分发到 ai 节点并合并答案 |
| `distnet agent status/start/stop/add/set/del/ai` | Agent 状态与节点管理 |
| `setip <ip>` | 配置 IP(SLIRP 下内核会自动 DHCP,一般无需手动) |

任务类型 `<type>` 支持 `fib` / `sum` / `echo`。已验证链路:

```
login nexos nexos
distnet nodes 10.0.2.2              -> discovered 10.0.2.2, total 1
distnet scheduler 10.0.2.2 fib 30   -> RESULT: RESULT 1 ok 832040
```

### 分布式推理(模型分片)

`tools/distnet_shard_orchestrator.py` + `tools/distnet_shard_node.py` 实现
**多节点协同推理同一个模型**,对应五项能力:

| 能力 | 实现 |
|------|------|
| 模型分片 | 按层切成连续区间,每个节点持有一段(`SHARD <job> <s> <e> ...`) |
| 分布式推理 | 激活值沿节点链逐段前传,末节点产出结果 |
| 动态负载均衡 | 分片边界按各节点算力权重 `weight` 比例划分(算力大的多分层) |
| 中间结果传输 | `AACT` 在节点间传激活值;`CKPT` 回传检查点给调度器 |
| 推理容错 | 心跳探测;节点掉线则剔除 → 重新分片 → 从最近检查点续跑 |

线协议(文本 UDP):`QUERY/BEACON`、`SHARD/SHARDOK`、`AACT`、`CKPT`、`OUT`、`PING/PONG`。

```bash
# 起 3 个分片节点(算力权重 1 / 2 / 4)
python tools/distnet_shard_node.py --port 5501 --weight 1 --dim 16 &
python tools/distnet_shard_node.py --port 5502 --weight 2 --dim 16 &
python tools/distnet_shard_node.py --port 5503 --weight 4 --dim 16 &
# 编排器:12 层模型切给这 3 个节点
python tools/distnet_shard_orchestrator.py --port 5500 --layers 12 --dim 16 \
       --nodes 127.0.0.1:5501,127.0.0.1:5502,127.0.0.1:5503
```

分片结果按算力 1:2:4 划分,节点掉线时自动恢复:

```
[orch] discovered: 5501(w=1), 5502(w=2), 5503(w=4)
[orch] job job1 shards: 5501:L0-0 | 5502:L1-3 | 5503:L4-11
[orch] !! node 5503 (L4-11) dead -> reshard
[orch] resume from checkpoint layer 3
[orch] job job1 shards: 5501:L4-5 | 5502:L6-11
[orch] job job1 done (2 shards)
```

验证:`python tools/_shard_demo.py`(自动起 3 节点 + 编排器,中途 kill 掉正在
计算的节点,验证重新分片与检查点续跑;恢复后的结果与无故障基线**逐位一致**)。

#### 在 Agent Forge 里使用(桥做协议路由)

Forge 的**分布式推理**节点发出 `forge shard <层数> <维度>`。`nexos_bridge.py`
识别 `forge ` 前缀后**不下发内核**,而是路由到宿主侧分片编排器 —— 桥在此充当
协议翻译/路由层(内核区已无空间放分片引擎):

```
Forge 节点 ──ws://8765──▶ nexos_bridge.py ──▶ distnet_shard_orchestrator
                                                   └─▶ 5501/5502/5503 分片节点
```

开箱即用:桥会自动拉起 3 个分片节点(算力权重 1/2/4),无需手工启动。
另有 `forge shard-nodes` 查看各节点存活状态。

```powershell
# 演示容错:让每层慢一点，好在推理途中 kill 节点
$env:SHARD_DELAY="0.5"
python tools\nexos_bridge.py
# 浏览器 Agent Forge → 示例 →「分布式推理」→ ▶ Run
```

> ⚠️ 已修复的两处同类陷阱(改动时勿回退):节点冷启动比固定 sleep 慢,编排器会
> 抢跑导致 `no shard nodes discovered`(改为轮询确认就绪);以及上面的
> `WSAECONNRESET` —— 分片节点也会因给已死下一跳发包而退出,表现为**一个节点掉线
> “传染”整条链**,节点与编排器两侧都必须捕获后 `continue`。

> **层算子说明**:当前是**确定性合成层**(`act = tanh(W_i·act + b_i)`,权重由
> `(seed, layer)` 生成),用于在无 GGUF 权重时验证分片/激活路由/容错这套机制。
> 嵌入真实模型后,只需把 `distnet_shard_node.py` 的 `apply_layers()` 换成真实层
> 算子,其余(分片、负载均衡、检查点、容错)完全复用。

> ⚠️ **Windows UDP 陷阱**(已修复,改动时勿回退):给已挂掉的节点发过 UDP 后,
> 对端回 ICMP 端口不可达,**下一次 `recvfrom()` 会抛 `WSAECONNRESET`**。若在收包
> 循环里 `break`,编排器会在节点挂掉的瞬间“失聪”——能检测故障却收不到恢复结果。
> 必须捕获后 `continue`。

### 计算节点从哪来

- **宿主 peer(推荐,单 VM 即可)**:`python tools/distnet_host_peer.py`
  在宿主起一个真实计算节点(UDP 5455/5456)。SLIRP 下宿主地址是 `10.0.2.2`。
- **第二台 VM(`-Fabric`)**:`.\run_nexos.ps1 -Ops -Fabric` 会启动
  `tools/nexos_l2hub.py`(L2 交换 hub)+ 两台 VM(调度器 + 计算节点),
  计算节点由 `tools/distnet_compute_driver.py` 自动登录并保持 `distnet compute` 在线。

### 关键约束:广播与网卡

- **内核只实现了 NE2000 ISA 驱动**(I/O 0x300 轮询),**不支持 virtio-net**。
  用 `-device virtio-net-pci` 启动时内核检测不到网卡(`netinfo` 的 MAC 会是全
  `FF:FF:FF:FF:FF:FF`),所有网络功能形同虚设。必须用 `-device ne2k_isa`。
- QEMU **点对点** `socket` netdev **不洪泛 L2 广播帧**,SLIRP(`-netdev user`)
  也不把 guest 广播转发到宿主。因此在 SLIRP 下要用**单播**(`distnet nodes <ip>`);
  多 VM 广播发现需要 `nexos_l2hub.py` 在宿主做帧洪泛。

## Web 控制台与串口桥(真 VM 后端)

浏览器 UI 不是模拟器,它通过 WebSocket 桥直连真实 QEMU 虚拟机内核:

```
浏览器(nexos-desktop.html)
   │  WebSocket  ws://127.0.0.1:8765
   ▼
tools/nexos_bridge.py       ← 桥:WebSocket 帧 ↔ 串口字节流
   │  TCP  127.0.0.1:4321
   ▼
QEMU 虚拟机串口 → NexOS 内核(kmain / kmain64)
```

启动:

```powershell
cd bootloader
.\run_nexos.ps1 -Ops                  # 启动 VM(NE2000)+ 桥
python tools\distnet_host_peer.py     # 可选:宿主计算节点
# 浏览器打开 win11-ui/nexos-desktop.html
#   → 右下角 conn → Connect → 用内核账号登录(nexos/nexos、root/admin、guest/guest)
```

- 登录是**真实校验**:前端不持有账号库,把 `login <user> <pass>` 发给内核,
  由内核比对影子文件的密码哈希,失败则不允许进入桌面。
- 面板里的命令(含 `distnet`)都经桥下发到真内核执行,输出原样回传。
- 早期曾有一个替代内核的 Node 假后端(`win11-ui/nexos-server.js`),**已删除**;
  现在只有“真 VM + 桥”一条链路,没有任何本地模拟回退。

## Visual Agent Forge(可视化 Agent 工场)

桌面 **Agent Forge** 图标打开后是一个节点图编辑器:**拖 → 连 → 运行**,不写代码。

- 左侧点节点类型添加;点节点**绿点(输出)→ 目标节点蓝点(输入)**连线;
  拖标题栏移动,标题栏 `✕` 删除。
- **Compile** 把图翻译成内核命令序列并显示;**▶ Run** 逐条下发到真内核执行。
- **示例**里的一键图(文件链 `mkfs → touch → ls`、算力链 `distnet scheduler … fib 30`)
  使用的都是实测通过的真实命令。
- 模板可 Save/Load(localStorage),图会自动保存。

节点 → 内核命令映射:

| 节点 | 生成的内核命令 |
|------|----------------|
| 思考 / AI | `ask64 "<提示词>"`(可切 `ai` / `ask`) |
| 搜索 / Net | `net http <url>` |
| 文件 / FS | `ls` / `cat <p>` / `touch <p>` / `rm <p>` / `mkfs` |
| 执行 / ELF | `run <程序>` |
| 算力 / Distnet | `distnet scheduler <ip> <job> <n>` |
| 分布式推理 | `forge shard <层数> <维度>`(多节点协同推理同一模型) |
| 判断 / If | 判断上一步输出是否包含文本,否则跳过下一个节点 |
| 循环 / Loop | 把其后的节点重复 N 次 |
| 输出 / Out | 把结果打到日志面板 |

控制语义说明:内核 shell 没有“图解释器”,所以 **if / loop 由前端解释**,但下发的
每一步仍是真实内核命令;线性链则直接等价于一段 `.sh`。

> 已知限制:当前镜像**未嵌入 GGUF 模型**时,“思考”节点只能走通切换 64 位与加载
> 管线,拿不到真实回答(需 `make MODEL_GGUF=<路径>` 重建)。文件节点在 SFS 未格式化
> 时 `ls` 会报 `MKFS not formatted`,先加一个 `mkfs` 节点即可(这两点在节点参数区
> 都有红字提示)。

## 用户系统

用户数据库持久化在 MKFS 根目录的 `shadow` 文件
(`name:uid:gid:group:hash\n`),密码用 **FNV-1a 32 位哈希**(盐=用户名,
8 位 hex)存储,不存明文。首次启动若无用户自动播种 `root/admin`(uid 0)
与 `guest/guest`(uid 1000)。`useradd`/`deluser`/`passwd`/`login` 都会
更新 `shadow` 并写回数据盘。

## 权限系统

- 9 位标准 `rwxrwxrwx`,权限表持久化在 MKFS 根目录 `permdb`
  (`name:uid:gid:mode3`);`FileEntry.reserved` 冗余存低 8 位。
- 鉴权顺序:root(uid 0)放行 → owner 位 → group 位 → other 位。
- `cat`(r)、`rm`/`write`/`touch`(w)自动检查;新建文件/目录默认
  0644/0755,归属创建者。
- `sudo <cmd>`:提示当前用户密码,验证通过后以 effective uid=0 执行,
  命令结束自动降权。

## 文件系统

### MKFS — 自制可写文件系统

```
LBA 512:     超级块   magic="MKFS", version, file_count, free_lba, total_sectors
LBA 513-528: 文件表   16 扇区, 每扇区 16 条目 = 最多 256 文件
LBA 529+:    数据区   启动盘 271 扇区 / 数据盘 15200 扇区, 顺序分配
```

文件条目(32 字节):`name[20] + size(4) + start_lba(4) + type(1) + parent(2) + reserved(1)`。

- `mkfs` 格式化:写魔数、清表、重置 `free_lba`;数据盘用大数据区。
- `write` 逐行输入(`>>` 提示符),空行结束保存;`create` 顺序分配扇区,
  同名覆盖;`mkdir`/`cd` 支持目录层级。
- **数据盘自动检测**:启动时扫描 ATA 硬盘(排除 ATAPI CD-ROM),
  找到即把 MKFS 写在那里 —— 重启后文件仍在,启动介质可只读。
- `shadow` / `permdb`(用户库 / 权限表)也存这里,随用户操作实时更新。

### SFS — 兼容只读文件系统

```
LBA 800:     超级块   magic="SFS", version, file_count, data_start
LBA 801-816: 目录     16 扇区, 最多 256 文件条目
LBA 817+:    数据区   按文件顺序排列, 每文件按扇区对齐
```

`sfs_files/` 中的文件在 `make` 时由 `tools/sfs_gen.py` 打包,格式与
MKFS 文件条目兼容,支持任意类型(`.sh`/`.txt` 等),文件名最长 23 字符。

当前包含:`hello.sh`(示例脚本)、`test.sh`(测试脚本)、`welcome.txt`。

### 脚本执行

`run`/`runfs` 逐行读取并执行 `.sh`:跳过空行与 `#` 注释,每条命令以
黄色 `> ` 前缀回显后调用 `run_command`;脚本内命令不污染历史
(`g_in_script` 标志)。

```
--- running SFS: hello.sh ---
> echo Hello from SFS script!
Hello from SFS script!
> about
MiniOS v2.0  -  C++ freestanding kernel
...
--- end of script ---
```

## GUI 桌面

输入 `gui` 进入 Win11 Portal 风格桌面(1024×768 32 位):

- 黑色顶栏:Start 按钮 + 运行指示 + 中文欢迎语 + 时钟/内存指示
- Portal 桌面:搜索栏 + 8 个彩色应用图标(Control/Files/Tasks/Memory/
  Terminal/Browser/Calc/About)+ 三卡片 + Home/Apps/System/Tools
- 可拖动窗口:Terminal(支持中文 + IME)/浏览器/计算器/任务管理器/
  控制面板/文件浏览器/About
- `run xxx.bat/.exe/.ps1` 在 GUI 中打开并执行,输出显示在窗口内

**VBE 适配**:BIOS 下 QEMU Cirrus/VMSVGA 没有经典 VBE BIOS 时,内核
自动探测 **BGA ports(0x1CE/0x1CF)** 合成 VBE info(1024×768×32,
LFB 0xFD000000),因此任何 QEMU/VirtualBox 显卡都能进 GUI;UEFI 路径
走 GOP + shadow framebuffer。

## 中文支持

- GB2312 16×16 点阵字库(387 个 UI 常用字),由 `tools/gen_zfont.py`
  从 SimSun 渲染(22px → LANCZOS 缩放到 16×16 → 阈值二值化)嵌入内核。
- `draw_text_utf8` / `draw_text_utf8_transparent`:UTF-8 3 字节 → Unicode
  → 二分查找字库 → 16×16 blit;ASCII 走原 8×16 字体,中英文混排。
- 顶栏 `欢迎使用`、Terminal 输出/输入均支持中文。

## 拼音输入法(IME)

- `tools/gen_ime_dict.py`:pypinyin 把 387 字转拼音,输出 `ime_dict.h`
  (按拼音排序,前缀匹配)。
- GUI Terminal 内直接可用:

| 按键 | 行为                                     |
|------|------------------------------------------|
| `a-z` | 进入拼音状态,显示候选条(紫拼音+黄编号+白字) |
| `1-9` | 选第 N 个候选,UTF-8 写入输入行            |
| `空格` | 选第 1 个候选                            |
| `退格` | 删最后一个拼音字符                        |
| `ESC` | 取消 IME(不退出 GUI)                     |

## 键盘 / 鼠标 / 终端

- **键盘**:`0x64` 状态口,`0x60` 读 Set 1 扫描码;支持 Backspace/Enter/
  Tab/Shift/CapsLock/方向键/Home/End/PgUp/PgDn/Ctrl+C(复制或中止)/
  Ctrl+V(粘贴)/Ctrl+L(聚焦)/Ctrl+↑↓(剪贴板历史)。
- **鼠标**:PS/2 Intellimouse(4 字节包,200/100/80 采样序列启用滚轮);
  左键拖选复制,右键聚焦输入,滚轮翻页。
- **终端**:200 行环形回看缓冲,25 行窗口;`m_view` 视口 + `m_at_bottom`
  贴底;翻页不破坏历史;光标用 0x3D4/0x3D5 定位,离开底部自动隐藏;
  任意输入自动 snap 回底部;支持 framebuffer console 模式(BGA/OVMF)。

## BIOS 引导流程

1. **Stage1**(`0x7C00`,16 位实模式):保存启动盘号,INT 13h AH=42h 从
   LBA 1 读 32 扇区 Stage2 到 `0x8000`,远跳转。
2. **Stage2**(`0x8000`,16 位实模式):从 LBA 33 读内核到 `0x10000`;
   端口 `0x92` 开 A20;加载 GDT;置 CR0.PE 切保护模式;远跳转
   `CODE_SEG:init_pm` 刷流水线、装段寄存器,跳到 `0x10000`。
3. **内核**(`0x10000`,32 位 PM):`entry.asm` 设栈 → `kmain`:
   硬件探测 → VBE 探测(BGA fallback)→ 终端/键盘/鼠标/文件系统
   → 用户库加载 → 强制登录 → shell 循环。

## UEFI 引导流程

1. **OVMF**:扫描 ESP 启动 `/EFI/BOOT/BOOTX64.EFI`(CPU 在长模式)。
2. **bootuefi.c**(64 位):LoadedImage 协议 → SimpleFileSystem 读
   `kernel.bin` → `AllocatePages(AllocateAddress, 0x10000)` 复制内核
   → 取内存映射 → `ExitBootServices`。
3. **enter_kernel.S**:RIP 相对 LEA 建 GDTR → LGDT → LRETQ 远返回
   32 位**兼容模式**段(长模式内执行 32 位指令,UEFI 恒等映射页表保持
   有效)→ 装平坦段、设栈 `0x90000` → 远跳 `0x10000`。
4. **内核**(32 位兼容模式):与 BIOS 路径相同;额外调
   `vga_set_text_mode()` 把 VGA 从 OVMF 图形模式切回文本模式 3。

### UEFI 调试

串口(0x3F8)输出标记:`[UEFI]`(bootloader 各阶段)、`E`(enter_kernel
入口)、`G`(GDT 已加载)、`3`(进入 32 位兼容模式)、`S`(内核 `_start`)、
`[K1]`~`[K6]`(kmain 各阶段)、`[K32]`(进入 GUI)。

### 设计决策:兼容模式 vs 完整模式切换

UEFI 启动后 CPU 在长模式,而内核是 32 位代码。**兼容模式方案**用
`L=0, D=1` 段在长模式内执行 32 位指令,UEFI 页表保持有效,所有
I/O/VGA/内存行为与 32 位保护模式一致,避免完整切换(关分页→关 PAE→
关 LME)的三重错误风险。CR0/CR4/EFER 仍保持长模式配置,对内核透明。
# 开源协议（MIT License）

NexOS 采用 **MIT 许可证**——一款 OSI 认证的宽松开源协议，具有明确法律效力，且**允许商业使用**（已删除原自定义协议中的“禁止商业使用”与“最终解释权归项目方”等削弱法律确定性的条款）。权威英文文本见仓库根目录 `LICENSE`；以下为便于阅读的中文释文。

> 采用 MIT 许可证意味着：任何人都可自由使用、复制、修改、合并、发布、分发、再许可乃至**出售**本软件，只需在副本中保留版权声明与许可声明；软件按“现状”提供，作者不承担任何担保或责任。

## MIT License（英文权威文本）

Copyright (c) 2026 transformer1155

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

## 中文释文

- **授予权利**：任何人可出于任何目的（含商业目的）使用、复制、修改、合并、发布、分发、再许可、销售本软件及其衍生版本。
- **唯一条件**：在所有副本或实质性部分中保留上述版权声明与许可声明。
- **免责**：本软件按“现状”提供，不提供任何明示或暗示担保；作者不对因使用本软件产生的任何损失承担责任。
- **贡献者许可**：向本仓库提交代码即表示同意按本 MIT 许可证授权。

---

## <a id="english-version"></a> 🇬🇧 English Version

# NexOS — Self-built x86 Operating System (Kernel · C# Desktop Shell · Web Console · AI · Distributed Compute)

NexOS is a from-scratch **x86 operating system**. It started as a C++ kernel + two-stage bootloader with dual BIOS/UEFI boot and 32/64-bit protected-mode switching, and has evolved into a **full-stack system fusing a graphical desktop, on-device LLM inference, a distributed compute network, and Windows/Linux compatibility**:

- **Kernel & boot**: Stage1→Stage2 two-stage loader; 32-bit protected-mode kernel `kernel.cpp` + 64-bit long-mode kernel `kernel64.cpp`; UEFI/BIOS paths converge on the same C++ entry.
- **GUI desktop**: `gui.cpp` draws a Win11-style desktop on the framebuffer; on top sits a **C# managed shell** (`csharp/NexOS.Forms` / `NexOS.Core`, `csharp/apps/Shell` implements the window manager, menus, tray, login and AI Agent via `Desktop.cs`/`Browser.cs`/`Login.cs`/`Shell.cs`/`AiAgent.cs`).
- **Web console**: `win11-ui/` provides a browser-based Win11 desktop + distributed-network panel + Visual Agent Forge, talking to the **real QEMU kernel** over a WebSocket bridge (`tools/nexos_bridge.py`) through the serial port — login/commands/compute tasks execute for real (no local simulation).
- **AI**: Markov + GPT-style text engine (`ai_engine.cpp`) + built-in **Qwen2-0.5B (Q4_K_M) GGUF Transformer inference engine** (`gguf_infer.cpp`, `ask64` switches to 64-bit streaming inference); `agent/` agents + Agent Forge visual orchestration.
- **Distributed compute**: in-kernel `distnet.cpp` (UDP discovery + scheduling + result collection) + standalone runtime `dcn/` (crypto/discovery/sched/transport/wifi) can shard inference/compute tasks to multiple VMs / host nodes for merged inference.
- **Compat layers**: self-built **PE/Win32 subsystem** (`winloader.cpp` + `win32.cpp`, already runs a self-built IE browser); **Linux binary compatibility** (`linux_compat.cpp` + `linux_root/`, runs ELF/dynamically-linked programs); **Mono runtime port** (`mono_port/`, runs .NET managed code on NexOS).
- **System facilities**: dual file systems MKFS (writable, dedicated data-disk persistence) + SFS (read-only); user/permission/sudo; network stack (NE2000 + HTTP); Chinese bitmap font + pinyin IME.

> **One-line positioning**: a teaching-grade **full-stack operating system** built on a self-built C++ kernel, fusing a C# managed desktop shell, a browser-based web console, on-device LLM inference, and a distributed compute network.

## Features

- **Boot & kernel**: dual BIOS/UEFI boot; 32-bit `kernel.cpp` and 64-bit `kernel64.cpp` (long mode) dual kernels; `switch64`/`switch32` switching.
- **Terminal & shell**: scrollback terminal + clipboard (mouse select / middle-click paste / Ctrl+C/V); PS/2 keyboard/mouse drivers; PowerShell-style shell, 48+ commands.
- **Users & permissions**: login / multi-user / password hashing (shadow file persistence); 9-bit `rwxrwxrwx` permissions; `sudo` elevation.
- **File systems**: MKFS (writable, dedicated data-disk persistence) + SFS (read-only) + VFS; NTFS read-only browsing; `.sh` script execution.
- **GUI desktop**: `gui.cpp` Win11-style framebuffer desktop + **C# managed shell** (`NexOS.Forms`/`NexOS.Core`) window manager; on-demand launch, BGA fallback for any GPU.
- **Chinese & IME**: GB2312 16×16 bitmap font (387 glyphs) + pinyin IME (pinyin → pick → UTF-8 in GUI Terminal).
- **Web console**: `win11-ui/` browser Win11 desktop + distributed-network panel + Agent Forge over WebSocket bridge to the real kernel.
- **AI inference**: `ai_engine.cpp` (Markov+GPT) text generation; `gguf_infer.cpp` built-in Qwen2-0.5B GGUF Transformer, `ask64` streaming inference; `agent/` agents.
- **Distributed compute**: `distnet.cpp` UDP discovery/scheduling + `dcn/` runtime (crypto/sched/transport), sharded tasks across nodes for merged inference.
- **Compat layers**: self-built PE/Win32 subsystem (`winloader.cpp`+`win32.cpp`, runs self-built IE); Linux binary compatibility (`linux_compat.cpp`+`linux_root/`); Mono runtime port (`mono_port/`).
- **Networking**: NE2000 driver + TCP/IP + HTTP server (reachable from a browser after `netstart`).

## Top-level Directories & Submodules

| Directory / File | Role |
|------------------|------|
| `boot.asm` `stage2.asm` `entry.asm` `entry64.asm` | Stage1/Stage2 loader, 32/64-bit kernel entry stubs |
| `kernel.cpp` `kernel64.cpp` `switch32to64.asm` `switch64to32.asm` `smp_impl64.cpp` | 32/64-bit kernels, mode switching, SMP |
| `gui.cpp` | Framebuffer GUI desktop + Chinese rendering + pinyin IME |
| `winloader.cpp` `win32.cpp` | Self-built PE/Win32 subsystem (load & run Windows exe) |
| `net.cpp` | NE2000 NIC driver + TCP/IP + HTTP server |
| `ai_engine.cpp` `gguf_infer.cpp` `gguf*.{c,h}` `ai_*` | AI text generation + Qwen2 GGUF 64-bit inference engine |
| `distnet.cpp` `distnet.h` | In-kernel distributed compute: UDP discovery + scheduling + compute nodes/Agent |
| `linux_compat.cpp` `linux_compat.h` `linux_root/` | Linux binary (ELF/dynamically-linked) compat layer |
| `uefi/bootuefi.c` `uefi/enter_kernel.S` `linker.ld` `linker64.ld` | UEFI boot & 32/64-bit linker scripts |
| `csharp/` | **C# managed shell**: `NexOS.Core`/`NexOS.Forms` API + `apps/Shell` (desktop/window/browser/login/AI Agent) + `winhost` host |
| `win11-ui/` | **Web console**: browser Win11 desktop + distributed-network panel + Agent Forge (`nexos-desktop.html`) + WinUI 3 standard controls/tokens |
| `dcn/` | **Distributed compute runtime**: `dcn_*` (crypto/discovery/sched/transport/wifi/kernel) + `rt/` + `build_qemu/` |
| `mono_port/` | **Mono runtime port**: `pal/` (platform abstraction) + build output, runs .NET managed code on NexOS |
| `agent/` | AI agent implementation (works with `ai_engine`/`gguf_infer`/`distnet`) |
| `tools/` | Build & ops scripts: `sfs_gen.py`, `embed_model.py`, `gen_zfont.py`, `gen_ime_dict.py`, `nexos_bridge.py`, `nexos_l2hub.py`, `distnet_host_peer.py`, `check_k64_fit.sh`, `analyze_login.py`/`analyze_ppms.py`, etc. |
| `sfs_files/` `docs/` `Makefile` `test.sh` `test_uefi.sh` | SFS sources, planning docs, build & headless tests |

## Memory & Disk Layout

```
Memory: 0x07C00  Stage1 (loaded by BIOS)
        0x08000  Stage2 (loaded by Stage1)
        0x10000  32-bit C++ kernel (loaded by Stage2 / UEFI bootloader)
        0x100000 64-bit kernel (loaded by switch64, long mode)
        0x90000  32-bit stack top

BIOS disk (LBA, sector=512B):
        LBA 0         boot.bin     (Stage1, 512B)
        LBA 1..32     stage2.bin   (Stage2, fixed 16KiB)
        LBA 33..544   kernel.bin   (kernel, max 256KiB)
        LBA 300..303  command history file (save/load, 2KiB)
        LBA 2048..    kernel64.bin (64-bit kernel)
        LBA 512       MKFS superblock (magic "MKFS")
        LBA 513..528  MKFS file table (16 sectors, 256 entries)
        LBA 529..799  MKFS data area (boot disk: 271 sectors / 135KB)
        LBA 800       SFS superblock (magic "SFS", pre-built by Makefile)
        LBA 801..816  SFS directory (16 sectors, 256 entries)
        LBA 817..1023 SFS data area (207 sectors, 103KB)

User data disk (second ATA hard disk / data.vhd, 8MB):
        LBA 512       MKFS superblock (magic "MKFS")
        LBA 513..528  MKFS file table (16 sectors, 256 entries)
        LBA 529..     MKFS data area (15200 sectors ≈ 7.5MB)
```

Stage2 is fixed at 16KiB, kernel starts at LBA 33. **MKFS data area is written to a dedicated user data disk** (first ATA HDD on Secondary/Primary), keeping the boot medium (ISO/CD) read-only; falls back to boot disk if no data disk is present (for backward compatibility). User files (including `shadow` user database and `permdb` permission table) thus persist across reboots.

Under UEFI, `BOOTX64.EFI` reads `kernel.bin` from ESP, copies it to `0x10000`, exits Boot Services, switches to 32-bit compatibility mode, and jumps — converging with the BIOS path.

## Dependencies

### BIOS Build

```bash
sudo apt install nasm g++ gcc-multilib make qemu-system-x86 python3
```

### UEFI Build

```bash
sudo apt install gnu-efi ovmf mtools
```

### Chinese Font / Pinyin Dictionary (optional, auto-generated during build)

- `gen_zfont.py` requires Pillow + a Chinese font (Windows SimSun / WenQuanYi etc.)
- `gen_ime_dict.py` requires pypinyin (`pip install pypinyin`)

## Build

```bash
make                 # Generate BIOS image build/os.img (with SFS)
make iso             # Generate CD-ROM image build/os.iso (BIOS+UEFI hybrid)
make uefi            # Generate UEFI image build/os_uefi.img
make sfs             # Generate only SFS image build/sfs.img
make data-vhd        # Generate user data disk build/data.vhd (8MB, pre-formatted MKFS)
make disasm          # View kernel entry and disassembly, verify _start at 0x10000
```

The SFS image is packed from `sfs_files/` by `tools/sfs_gen.py`. Chinese font and pinyin dictionary are auto-generated by `gen_zfont.py` / `gen_ime_dict.py` before `gui.cpp` compilation.

## Running / Testing

### BIOS

```bash
make test            # Headless auto-test: shell + paging + save/load + file system + scripts
make iso-run         # Run ISO image (os.iso)
```

### ISO + Data Disk (recommended, file persistence)

```bash
make iso-run-data    # -cdrom os.iso + -drive data.vhd (IDE secondary)
```

### UEFI

```bash
make uefi-run        # QEMU window (OVMF firmware)
make uefi-test       # Headless test: serial + screenshot verification
```

`make test` uses monitor `sendkey` to simulate input in 6 stages, dumping `0xB8000` for verification: help/echo echo, arrow key paging, save/load history persistence, MKFS CRUD operations, SFS directory listing / file reading / script execution. Currently also includes login step (`root`/`admin`).

`make uefi-test` captures serial output to verify: UEFI bootloader loads kernel → exits Boot Services → x64→32-bit compatibility mode → kmain → VGA text mode → Hello world. After OVMF exits, monitor reads VGA returning `0xFFFFFFFF`, so it switches to `screendump` screenshot to count non-black pixels and verify screen content.

## Shell

After boot, you are presented with a **login prompt**. Enter username/password (preset: `root`/`admin`, `guest`/`guest`). Upon success, you enter the shell with prompt `PS user@minios /path>`.

### Basic Commands

| Command | Description |
|---------|-------------|
| `help` | List all commands |
| `echo <text>` | Print text |
| `clear` / `cls` | Clear screen and scrollback history |
| `about` | System information |
| `history` / `h` | Show command history (plus loaded from disk) |
| `save` | Write command history to disk (LBA 300) |
| `load` | Read command history from disk |

### MKFS Commands (custom writable file system)

| Command | Description |
|---------|-------------|
| `mkfs` | Format MKFS (data disk or boot disk) |
| `ls` / `dir` | List files on MKFS |
| `cat` / `type` | Print MKFS file contents |
| `touch` / `ni` | Create empty file |
| `write <f>` | Write text line by line (empty line ends, max 8KB) |
| `rm` / `del` | Delete MKFS file |
| `copy` / `cp` | Copy file |
| `mkdir` / `md` | Create directory |
| `cd` / `sl` | Change directory |
| `pwd` / `gl` | Show current directory |

### SFS / Partition / Script Commands

| Command | Description |
|---------|-------------|
| `lsfs` | List files on SFS |
| `catfs <f>` | Print SFS file contents |
| `run <f>` | Execute `.sh` script on MKFS |
| `runfs <f>` | Execute `.sh` script on SFS |
| `part` / `mount` / `lsfat` / `fatinfo` | FAT32 partition management and browsing |

### User / Permission / sudo Commands

| Command | Description |
|---------|-------------|
| `whoami` / `id` | Show current user / uid / gid |
| `users` | List all users |
| `login <user>` | Switch user (asks for password) |
| `logout` | Log out, return to login prompt |
| `su <user>` | Switch user (default root, asks for password) |
| `useradd <name> [pw]` | Add user (root only, default password 123456) |
| `deluser <name>` | Delete user (root only, cannot delete root/current user) |
| `passwd [user]` | Change password (root can change any user; users need old password) |
| `chmod <mode> <file>` | Change file permissions (e.g., 600/644/755, owner or root) |
| `stat <file>` | Show owner / gid / mode (rwxrwxrwx + octal) |
| `sudo <cmd>` | Enter password to elevate and run a single command as root |

### Kernel / GUI / Network / AI / Power Commands

| Command | Description |
|---------|-------------|
| `gui` | Enter GUI desktop (BGA fallback, works on any GPU) |
| `switch` / `switch64` | Switch to 64-bit kernel (long mode) |
| `netinfo` / `netstart` | Network status / start NE2000 + HTTP server |
| `ai` / `generate` / `agent` | AI text generation (initial model / generate / Agent) |
| `meminfo` / `memtest` / `pagetest` | Memory / page table diagnostics |
| `run xxx.bat/.exe/.ps1` | Open and execute via winloader in GUI |
| `shutdown` / `reboot` | Power off / reboot |

## User System

User database is persisted in `shadow` file at the root of MKFS (`name:uid:gid:group:hash\n`). Passwords are hashed with **FNV-1a 32-bit hash** (salt = username, 8 hex digits) — plaintext passwords are never stored. On first boot, if no users exist, it automatically seeds `root/admin` (uid 0) and `guest/guest` (uid 1000). `useradd`/`deluser`/`passwd`/`login` all update `shadow` and write back to the data disk.

## Permission System

- Standard 9-bit `rwxrwxrwx` permissions, persisted in `permdb` at MKFS root (`name:uid:gid:mode3`); `FileEntry.reserved` redundantly stores lower 8 bits.
- Permission check order: root (uid 0) bypass → owner bits → group bits → other bits.
- `cat` (r), `rm`/`write`/`touch` (w) automatically check permissions; new files/directories default to 0644/0755, owned by the creator.
- `sudo <cmd>`: prompts for current user password, upon verification executes as effective uid=0, and drops privileges immediately after the command finishes.

## File Systems

### MKFS — Custom Writable File System

```
LBA 512:     Superblock   magic="MKFS", version, file_count, free_lba, total_sectors
LBA 513-528: File table   16 sectors, 16 entries/sector = max 256 files
LBA 529+:    Data area    Boot disk: 271 sectors / data disk: 15200 sectors, sequential allocation
```

File entry (32 bytes): `name[20] + size(4) + start_lba(4) + type(1) + parent(2) + reserved(1)`.

- `mkfs` formatting: writes magic, clears table, resets `free_lba`; data disk uses the larger data area.
- `write` inputs line by line (`>>` prompt), saves on empty line; `create` allocates sectors sequentially, overwrites on name conflict; `mkdir`/`cd` supports directory hierarchy.
- **Auto data disk detection**: on boot, it scans ATA HDDs (excluding ATAPI CD-ROM) and uses the first found for MKFS — files persist across reboots, boot medium can be read-only.
- `shadow` / `permdb` (user database / permission table) also reside here and update in real-time.

### SFS — Compatible Read-Only File System

```
LBA 800:     Superblock   magic="SFS", version, file_count, data_start
LBA 801-816: Directory    16 sectors, max 256 file entries
LBA 817+:    Data area    Files stored sequentially, sector-aligned
```

Files in `sfs_files/` are packed by `tools/sfs_gen.py` during `make`. The format is compatible with MKFS file entries, supports any file type (`.sh`/`.txt` etc.), max filename length 23 characters.

Currently includes: `hello.sh` (example script), `test.sh` (test script), `welcome.txt`.

### Script Execution

`run`/`runfs` read and execute `.sh` line by line: skip empty lines and `#` comments, echo each command with yellow `> ` prefix before executing via `run_command`. Commands inside scripts do not pollute command history (`g_in_script` flag).

```
--- running SFS: hello.sh ---
> echo Hello from SFS script!
Hello from SFS script!
> about
MiniOS v2.0  -  C++ freestanding kernel
...
--- end of script ---
```

## GUI Desktop

Enter `gui` to launch the Win11 Portal style desktop (1024×768 32-bit):

- Black top bar: Start button + running indicator + Chinese welcome text + clock/memory indicator
- Portal desktop: search bar + 8 colored app icons (Control/Files/Tasks/Memory/Terminal/Browser/Calc/About) + 3 cards + Home/Apps/System/Tools
- Draggable windows: Terminal (with Chinese + IME support) / Browser / Calculator / Task Manager / Control Panel / File Explorer / About
- `run xxx.bat/.exe/.ps1` opens and executes in a GUI window, with output displayed inside

**VBE Adaptation**: Under BIOS, when QEMU Cirrus/VMSVGA lacks classic VBE BIOS, the kernel auto-probes **BGA ports (0x1CE/0x1CF)** and synthesizes VBE info (1024×768×32, LFB 0xFD000000). Thus, any QEMU/VirtualBox GPU can enter GUI. UEFI path uses GOP + shadow framebuffer.

## Chinese Support

- GB2312 16×16 bitmap font (387 commonly-used UI characters), generated by `tools/gen_zfont.py` from SimSun (22px → LANCZOS scaled to 16×16 → threshold binarization) and embedded in the kernel.
- `draw_text_utf8` / `draw_text_utf8_transparent`: UTF-8 3-byte → Unicode → binary search font → 16×16 blit; ASCII uses original 8×16 font, enabling mixed Chinese/English rendering.
- Top bar `欢迎使用`, Terminal output/input all support Chinese.

## Pinyin IME

- `tools/gen_ime_dict.py`: uses pypinyin to convert 387 characters to pinyin, outputs `ime_dict.h` (sorted by pinyin, prefix matching).
- Directly usable within GUI Terminal:

| Key | Behavior |
|-----|----------|
| `a-z` | Enter Pinyin mode, show candidate bar (purple pinyin + yellow number + white character) |
| `1-9` | Select Nth candidate, write UTF-8 into input line |
| `Space` | Select 1st candidate |
| `Backspace` | Delete last pinyin character |
| `ESC` | Cancel IME (doesn't exit GUI) |

## Keyboard / Mouse / Terminal

- **Keyboard**: `0x64` status port, `0x60` reads Set 1 scancodes; supports Backspace/Enter/Tab/Shift/CapsLock/arrows/Home/End/PgUp/PgDn/Ctrl+C (copy or abort) / Ctrl+V (paste) / Ctrl+L (focus) / Ctrl+↑↓ (clipboard history).
- **Mouse**: PS/2 Intellimouse (4-byte packets, 200/100/80 sample sequences enabling scroll wheel); left-click drag to select copy, right-click to focus input, scroll wheel for paging.
- **Terminal**: 200-line ring scrollback buffer, 25-line window; `m_view` viewport + `m_at_bottom` stick-to-bottom; paging doesn't corrupt history; cursor positioned via 0x3D4/0x3D5, auto-hidden when leaving bottom; any input auto-snaps to bottom; supports framebuffer console mode (BGA/OVMF).

## BIOS Boot Flow

1. **Stage1** (`0x7C00`, 16-bit real mode): saves boot drive number, uses INT 13h AH=42h to read 32 sectors of Stage2 from LBA 1 to `0x8000`, far jumps.
2. **Stage2** (`0x8000`, 16-bit real mode): reads kernel from LBA 33 to `0x10000`; enables A20 via port `0x92`; loads GDT; sets CR0.PE to enter protected mode; far jumps to `CODE_SEG:init_pm`, flushes pipeline, loads segment registers, jumps to `0x10000`.
3. **Kernel** (`0x10000`, 32-bit PM): `entry.asm` sets up stack → `kmain`: hardware probing → VBE probing (BGA fallback) → terminal/keyboard/mouse/file systems → user database loading → mandatory login → shell loop.

## UEFI Boot Flow

1. **OVMF**: scans ESP and boots `/EFI/BOOT/BOOTX64.EFI` (CPU in long mode).
2. **bootuefi.c** (64-bit): uses LoadedImage protocol → SimpleFileSystem to read `kernel.bin` → `AllocatePages(AllocateAddress, 0x10000)` to copy kernel → gets memory map → `ExitBootServices`.
3. **enter_kernel.S**: RIP-relative LEA builds GDTR → LGDT → LRETQ far-return to 32-bit **compatibility mode** segment (executes 32-bit instructions within long mode, UEFI identity-mapped page tables remain valid) → loads flat segments, sets stack `0x90000` → far jumps to `0x10000`.
4. **Kernel** (32-bit compatibility mode): same as BIOS path; additionally calls `vga_set_text_mode()` to switch VGA from OVMF's graphics mode back to text mode 3.

### UEFI Debugging

Serial (0x3F8) outputs markers: `[UEFI]` (bootloader stages), `E` (enter_kernel entry), `G` (GDT loaded), `3` (entered 32-bit compatibility mode), `S` (kernel `_start`), `[K1]`–`[K6]` (kmain stages), `[K32]` (entered GUI).

### Design Decision: Compatibility Mode vs. Full Mode Switch

UEFI starts in long mode while the kernel is 32-bit code. The **compatibility mode approach** uses a `L=0, D=1` segment to execute 32-bit instructions within long mode, keeping UEFI page tables valid. All I/O/VGA/memory behavior is identical to 32-bit protected mode, avoiding the triple-fault risk of a complete mode switch (disable paging → disable PAE → disable LME). CR0/CR4/EFER remain in long mode configuration, which is transparent to the kernel.

---

# NexOS Open Source License

**NexOS Open Source License (MiniOS Open Source License)**  
Copyright (c) 2026 MiniOS Project Developers (hereinafter "Project Owner")

Permission is hereby granted, free of charge, to any person or organization obtaining a copy of this software and associated documentation files (the "Software"), to use, copy, modify, merge, publish, and distribute the Software and its derivative works, for the purposes of learning, research, teaching, personal interest, or non-commercial internal use, subject to the following conditions:

## 1. Definitions

1.1 "Software" refers to the source code, binary files, documentation, and related resources governed by this license.

1.2 "Derivative Work" refers to any new version created by modifying, extending, or porting the original Software.

## 2. Permitted Uses

2.1 **Learning and Research**  
Anyone may view, analyze, and run the Software for the purpose of learning operating system principles, programming techniques, or conducting scientific research.

2.2 **Modification and Extension**  
Anyone may modify, optimize, or enhance the Software and create Derivative Works.

2.3 **Distribution of Derivative Works**  
Anyone may distribute Derivative Works based on the Software, provided they comply with this license. When distributing, the Derivative Work must clearly indicate the modifications made and retain this license and copyright notice.

2.4 **Internal Use**  
Organizations may use the Software internally for educational, training, or internal technology validation purposes, without being subject to the commercial use restrictions.

## 3. Prohibited Uses

3.1 **Commercial Use**  
Without explicit written permission from the Project Owner, no one may use the Software or its Derivative Works for any commercial purpose, including but not limited to:

a) Charging fees or generating profit directly or indirectly through the Software;  
b) Selling the Software as part of a product or service;  
c) Integrating the Software into products intended for sale or rental;  
d) Using the Software to provide paid technical services, outsourcing, or consulting to commercial clients.

3.2 **Plagiarism or Misappropriation**  
No one may claim the Software or its Derivative Works as their own original creation, nor remove or alter copyright notices, author information, or the terms of this license.

3.3 **Malicious Use**  
No one may use the Software for any illegal activities, network security violations, or infringement of others' rights.

## 4. Requirements for Derivative Works

4.1 Any Derivative Work must retain the full text of this license or provide a clear link to it in a prominent location.

4.2 The distributor of a Derivative Work must clearly state the modifications made to the original Software, and ensure that the Derivative Work does not mislead others into believing it is directly affiliated with the original Project Owner.

4.3 Derivative Works are subject to the same terms of this license, especially the commercial use restrictions.

## 5. Disclaimer of Warranty

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE, OR SECURITY. IN NO EVENT SHALL THE PROJECT OWNER BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES ARISING OUT OF THE USE OF THE SOFTWARE.

## 6. Termination

Any violation of the terms of this license will result in the automatic termination of the rights granted herein. Violators must immediately cease use and destroy all copies.

## 7. Final Interpretation

The final right of interpretation of this license belongs to the Project Owner. The Project Owner reserves the right to modify the terms of this license as needed, with changes being notified through appropriate channels.

**Version: 1.0**  
**Effective Date: 2026-08-11**

---

<p align="right"><a href="#nexos--two-stage-bootloader-c-kernel">⬆ Back to top</a></p>
```

---
