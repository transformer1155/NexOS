# 二阶引导加载 C++ 内核

一个最小可运行的 x86 引导程序,支持 **BIOS** 和 **UEFI** 双引导路径。
Stage1 引导扇区 → Stage2 二阶引导 → 切换到 32 位保护模式 → 跳转到 C++ 内核。
内核带滚动回看终端、PS/2 键盘 + 鼠标驱动、命令行 shell、双文件系统
(MKFS 自制可写 + SFS 兼容只读)和 `.sh` 脚本执行,并能把命令历史持久化到磁盘。

## 目录结构

| 文件                 | 说明                                                         |
|----------------------|--------------------------------------------------------------|
| `boot.asm`           | Stage1,512 字节引导扇区,用 INT 13h 扩展读加载 Stage2        |
| `stage2.asm`         | Stage2,加载内核、开启 A20、建 GDT、切入 32 位保护模式        |
| `entry.asm`          | 内核入口桩(32 位),设栈后调用 C++ 的 `kmain`               |
| `kernel.cpp`         | C++ 内核:终端 + 键盘/鼠标 + ATA 磁盘 + shell + 双文件系统   |
| `linker.ld`          | 内核链接脚本,入口 `_start` 定位在 `0x10000`                 |
| `uefi/bootuefi.c`    | UEFI 引导程序(x86_64,gnu-efi):加载 kernel.bin、退出 Boot Services |
| `uefi/enter_kernel.S`| UEFI 模式切换:x86_64 长模式 → 32 位兼容模式 → 跳转内核      |
| `tools/sfs_gen.py`   | SFS 镜像生成器:将 `sfs_files/` 打包为只读文件系统镜像       |
| `sfs_files/`         | SFS 源文件目录:放入 `.sh` 脚本和文本文件,构建时自动打包    |
| `Makefile`           | 构建脚本:BIOS + UEFI + SFS 镜像生成                         |
| `test.sh`            | BIOS 无头测试:shell + 翻页 + save/load + 文件系统 + 脚本    |
| `test_uefi.sh`       | UEFI 无头测试:QEMU + OVMF + 串口 + 屏幕截图校验             |

## 内存与磁盘布局

```
内存:  0x07C00  Stage1 (BIOS 加载)
       0x08000  Stage2 (Stage1 加载)
       0x10000  C++ 内核 (Stage2 / UEFI bootloader 加载)
       0x90000  32 位栈顶

BIOS 磁盘 (LBA, 扇区=512B):
       LBA 0         boot.bin     (Stage1, 512B)
       LBA 1..32     stage2.bin   (Stage2, 固定 16KiB)
       LBA 33..128   kernel.bin   (内核, 最多 48KiB)
       LBA 256..259  命令历史文件  (save/load, 2KiB)
       LBA 512       MKFS 超级块   (魔数 "MKFS")
       LBA 513..528  MKFS 文件表   (16 扇区, 256 条目)
       LBA 529..799  MKFS 数据区   (271 扇区, 135KB)
       LBA 800       SFS 超级块   (魔数 "SFS", Makefile 预生成)
       LBA 801..816  SFS 目录     (16 扇区, 256 条目)
       LBA 817..1023 SFS 数据区   (207 扇区, 103KB)

UEFI 磁盘 (FAT12/16 ESP + 原始 SFS):
       /EFI/BOOT/BOOTX64.EFI   UEFI 引导程序
       /kernel.bin             C++ 内核
       LBA 800..               SFS 镜像 (与 BIOS 路径相同)
```

Stage2 被填充到固定 16KiB,因此内核起始 LBA(33)是确定的。MKFS 和 SFS
位于磁盘后半段,远离引导区与内核,互不干扰。

UEFI 路径下,`BOOTX64.EFI` 从 ESP 读取 `kernel.bin` 并复制到 `0x10000`,
退出 Boot Services 后切换到 32 位兼容模式跳转过去——与 BIOS 路径汇合。
SFS 镜像在 UEFI 镜像中也写入 LBA 800,内核通过同一 ATA 驱动读取。

## 依赖

### BIOS 构建

需要 `nasm`、`g++`(含 32 位 multilib)、`ld`、`objcopy`、`make`、`qemu-system-x86_64`、`python3`。

```bash
sudo apt install nasm g++ gcc-multilib make qemu-system-x86 python3
```

### UEFI 构建

额外需要 `gnu-efi`(UEFI 开发库)、`ovmf`(UEFI 固件模拟)、`mtools`(ESP 镜像工具)。

```bash
sudo apt install gnu-efi ovmf mtools
```

## 构建

```bash
make            # 生成 BIOS 镜像 build/os.img (含 SFS)
make uefi       # 生成 UEFI 镜像 build/os_uefi.img (含 SFS)
make sfs        # 仅生成 SFS 镜像 build/sfs.img
make disasm     # 查看内核入口与反汇编,确认 _start 在 0x10000
```

SFS 镜像由 `tools/sfs_gen.py` 从 `sfs_files/` 目录自动打包。修改
`sfs_files/` 中的文件后重新 `make` 即可更新镜像。

## 运行 / 测试

### BIOS

```bash
make run        # 在 QEMU 窗口中运行(需要图形环境),可手动敲键盘/滚鼠标
make test       # 无头自动测试:shell + 翻页 + save/load + 文件系统 + 脚本
```

### UEFI

```bash
make uefi-run   # 在 QEMU 窗口中运行(OVMF 固件,需要图形环境)
make uefi-test  # 无头自动测试:串口调试 + 屏幕截图校验
```

`make test` 启动无显示的 QEMU,通过 monitor 的 `sendkey` 模拟输入,
分 6 个阶段导出 `0xB8000` 的 VGA 内存并校验:

| 阶段    | 测试内容                                         |
|---------|--------------------------------------------------|
| Phase A | `help` + `echo hello` 回显                       |
| Phase C | 上方向键翻页 → 回到横幅 "Hello world"            |
| Phase D | 下方向键回底部 → 最新输出可见                    |
| Phase E | save 后重启 → load 读回命令历史(文件持久化)    |
| Phase F1| MKFS: mkfs 格式化、write 写入、cat 读取、ls 列出、rm 删除 |
| Phase F2| SFS: lsfs 列出、catfs 读取文件、runfs 执行 .sh 脚本 |

`make uefi-test` 用 QEMU + OVMF 启动 UEFI 镜像,捕获串口输出验证:
UEFI bootloader 加载内核 → 退出 Boot Services → x64→32 位模式切换 →
kmain 执行 → VGA 文本模式初始化 → Hello world 输出。
由于 OVMF 退出后 QEMU monitor 无法直接读 VGA 内存(返回 `0xFFFFFFFF`),
改用 `screendump` 截图统计非黑像素数来验证屏幕有内容。

## 命令行 shell

进入内核后是一个交互式 shell,提示符 `>`。内置命令:

### 基础命令

| 命令            | 作用                                   |
|-----------------|----------------------------------------|
| `help`          | 列出所有命令                           |
| `echo <text>`   | 打印文本                               |
| `clear` / `cls` | 清屏并清空回看历史                     |
| `about`         | 系统信息                               |
| `history`       | 显示本次(及从磁盘载入的)命令历史     |
| `save`          | 把命令历史写入磁盘文件(LBA 256)      |
| `load`          | 从磁盘文件读回命令历史                 |

### MKFS 命令(自制可写文件系统)

| 命令            | 作用                                   |
|-----------------|----------------------------------------|
| `mkfs`          | 格式化 MKFS 文件系统                   |
| `ls`            | 列出 MKFS 上的文件                     |
| `cat <f>`       | 打印 MKFS 文件内容                     |
| `touch <f>`     | 创建空文件                             |
| `write <f>`     | 逐行写入文本(空行结束,最大 8KB)     |
| `rm <f>`        | 删除 MKFS 文件                         |

### SFS 命令(兼容只读文件系统)

| 命令            | 作用                                   |
|-----------------|----------------------------------------|
| `lsfs`          | 列出 SFS 上的文件                      |
| `catfs <f>`     | 打印 SFS 文件内容                      |

### 脚本执行

| 命令            | 作用                                   |
|-----------------|----------------------------------------|
| `run <f>`       | 执行 MKFS 上的 `.sh` 脚本              |
| `runfs <f>`     | 执行 SFS 上的 `.sh` 脚本               |

## 文件系统

内核实现了两个文件系统,共存于同一磁盘:

### MKFS — 自制可写文件系统

MKFS(Mini Kernel File System)是一个自定义的简单文件系统,支持
创建、读取、写入、删除文件。结构如下:

```
LBA 512:     超级块   magic="MKFS", version, file_count, free_lba
LBA 513-528: 文件表   16 扇区, 每扇区 16 条目 = 最多 256 文件
LBA 529-799: 数据区   271 扇区 = 135KB, 顺序分配
```

文件条目(32 字节):`name[24] + size(4) + start_lba(4)`

- `mkfs` 格式化:写入魔数、清空文件表、重置数据区指针。
- `write` 命令进入逐行输入模式(`>>` 提示符),空行结束并保存。
- `create` 时顺序分配扇区(从 `free_lba` 开始),不回收已删除空间。
- 同名文件会被覆盖(先删除旧条目再创建新的)。

### SFS — 兼容只读文件系统

SFS(Simple File System)是一个预构建的只读文件系统,由 Makefile
在编译时从 `sfs_files/` 目录打包生成。结构与 MKFS 类似:

```
LBA 800:     超级块   magic="SFS", version, file_count, data_start
LBA 801-816: 目录     16 扇区, 最多 256 文件条目
LBA 817+:    数据区   按文件顺序排列, 每文件按扇区对齐
```

- `sfs_files/` 中的文件在 `make` 时由 `tools/sfs_gen.py` 打包。
- 内核通过 ATA PIO 读取,格式与 MKFS 文件条目兼容。
- 支持任意文件类型(`.sh` 脚本、`.txt` 文本等),文件名最长 23 字符。

当前 `sfs_files/` 包含:

| 文件           | 说明                              |
|----------------|-----------------------------------|
| `hello.sh`     | 示例脚本:echo + about            |
| `test.sh`      | 测试脚本:echo + about + history  |
| `welcome.txt`  | 文本文件:欢迎信息                |

### 脚本执行

`run` / `runfs` 命令从文件系统读取 `.sh` 脚本并逐行执行:

- 逐行解析,跳过空行和 `#` 开头的注释行。
- 每条命令以黄色 `> ` 前缀显示,然后调用 `run_command` 执行。
- 支持所有 shell 命令(`echo`、`about`、`history`、`ls`、`lsfs` 等)。
- 脚本执行期间的命令不会污染命令历史(通过 `g_in_script` 标志控制)。
- 脚本中可以调用任意已注册的命令,实现自动化测试和演示。

示例(`runfs hello.sh` 输出):
```
--- running SFS: hello.sh ---
> echo Hello from SFS script!
Hello from SFS script!
> about
MiniOS v2.0  -  C++ freestanding kernel
Two-stage boot, 32-bit protected mode.
PS/2 keyboard + mouse, ATA disk, VGA 80x25.
MKFS + SFS file systems, .sh script runner.
--- end of script ---
```

## 键盘 / 鼠标 / 翻页

保护模式下没有 BIOS,内核直接驱动硬件:

- **键盘**:`0x64` 状态口 bit0=有数据,`0x60` 读扫描码(Set 1)。
  支持 Backspace、Enter、Tab、空格、Shift、CapsLock,以及方向键
  (E0 扩展前缀:Up=0x48 / Down=0x50)。
- **鼠标**:PS/2 Intellimouse 模式(4 字节包),初始化时发送采样率
  200/100/80 序列启用滚轮。第 4 字节(Z 轴)正=向上、负=向下。
- **翻页**:鼠标滚轮上/下、键盘 Up/Down 都会滚动**回看视图**;视图离开
  底部时隐藏光标,任意输入自动回到底部(snap)。

## 滚动回看终端

终端维护一个 200 行的环形回看缓冲(`SCROLLBACK_LINES`),所有输出行
(含提示符与回显)都记录在内。屏幕是这缓冲的一个 25 行「窗口」:

- `m_view` 指向窗口顶端所在行;`m_at_bottom` 表示是否贴底。
- 贴底时,当前正在编辑的输入行显示在最后一行,硬件光标跟随输入位置。
- 写满后新行从底部进入、旧行上移(不再回卷覆盖);翻页时只移动窗口,
  不修改历史。
- 光标用 VGA 寄存器 `0x3D4/0x3D5` 定位,`0x0A/0x0B` 设为可见实心块;
  翻页离开底部时用 `0x0A` 的 bit5 禁用光标。

## BIOS 引导流程

1. **Stage1**(`0x7C00`,16 位实模式):保存启动盘号,用 INT 13h AH=42h
   从 LBA 1 读取 32 个扇区的 Stage2 到 `0x8000`,远跳转过去。
2. **Stage2**(`0x8000`,16 位实模式):从 LBA 33 读取内核到 `0x10000`;
   通过端口 `0x92` 开启 A20;加载 GDT;置 CR0.PE 切入保护模式;
   远跳转 `CODE_SEG:init_pm` 刷新流水线并装载段寄存器,最后跳到 `0x10000`。
3. **内核**(`0x10000`,32 位保护模式):`entry.asm` 设栈后调用 `kmain`,
   初始化终端/键盘/鼠标/文件系统,打印横幅,进入命令行 shell 循环。

## UEFI 引导流程

1. **OVMF 固件**:`qemu-system-x86_64` 加载 OVMF,扫描 ESP 找到
   `/EFI/BOOT/BOOTX64.EFI` 并启动。CPU 此时在 x86_64 长模式。
2. **UEFI bootloader**(`bootuefi.c`,64 位):通过 `LoadedImage` 协议
   获取设备句柄,打开 `SimpleFileSystem` 读取 `kernel.bin`;用
   `AllocatePages(AllocateAddress, ..., 0x10000)` 在物理地址 `0x10000`
   分配页面,逐字节复制内核过去;获取内存映射,调用 `ExitBootServices`。
3. **模式切换**(`enter_kernel.S`):
   - 用 RIP 相对 `LEA` 取 GDT 基址,在栈上构建 GDTR 并 `LGDT`——
     避免 PE 加载器可能不处理的绝对地址重定位。
   - 通过 `LRETQ` 远返回到 32 位**兼容模式**段(仍在长模式内,但执行
     32 位指令)。UEFI 的恒等映射页表保持有效,所有物理地址直接可用。
   - 装载平坦数据段、设栈 `0x90000`,远跳转到 `0x10000`。
4. **内核**(`0x10000`,32 位兼容模式):与 BIOS 路径完全相同——
   `entry.asm` 设栈后调用 `kmain`。内核额外调用 `vga_set_text_mode()`
   将 VGA 从 OVMF 的图形模式切换到文本模式 3(80×25)。

### UEFI 调试

整个引导链通过串口(端口 `0x3F8`)输出调试标记:

| 标记  | 含义                              |
|-------|-----------------------------------|
| `[UEFI]` | UEFI bootloader 各阶段消息     |
| `E`   | `enter_kernel` 入口               |
| `G`   | GDT 已加载                        |
| `3`   | 进入 32 位兼容模式                |
| `S`   | 内核 `_start` 入口                |
| `[K1]` | `kmain` 进入                     |
| `[K2]` | VGA 文本模式设置完成             |
| `[K3]` | 终端初始化完成                   |
| `[K4]` | 鼠标初始化完成                   |
| `[K5]` | Hello world 已写入终端           |
| `[K6]` | 文件系统初始化完成               |

`make uefi-test` 的串口输出示例:
```
[UEFI] UEFI bootloader -> C++ kernel
[UEFI] LoadedImage OK, DeviceHandle=...
[UEFI] kernel.bin loaded: 46908 bytes
[UEFI] Kernel copied to 0x10000
[UEFI] Exiting boot services...
EG3S[K1] kmain entered
[K2] VGA text mode set
[K3] terminal init done
[K4] mouse init done
[K6] filesystem init done
[K5] Hello world written
```

### 设计决策:兼容模式 vs 完整模式切换

UEFI 启动后 CPU 在 x86_64 长模式,而内核是 32 位代码。有两种方案:

1. **完整模式切换**:关闭分页 → 关 CR4.PAE → 关 EFER.LME → 回到真
   32 位保护模式。需要 UEFI 页表恒等映射所有内核访问的地址,否则三重错误。
2. **兼容模式**(本方案):用 32 位兼容段(`L=0, D=1`)在长模式内执行
   32 位指令,UEFI 页表保持有效。所有 I/O、VGA、内存访问行为完全相同。

方案 2 更简单且不会因页表/地址问题三重错误,代价是 CR0/CR4/EFER 仍保持
长模式配置——对 32 位内核透明,无功能影响。
