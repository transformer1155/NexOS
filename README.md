

---
<!-- ============================================================ -->
<!-- 语言切换导航栏（纯HTML + 锚点，无需JavaScript，100%兼容）    -->
<!-- ============================================================ -->

<div align="center">

# NexOS — 二阶引导加载 C++ 内核

[![Language](https://img.shields.io/badge/中文-简体-red)](#中文版本)
[![Language](https://img.shields.io/badge/English-README-blue)](#english-version)

</div>

---

<!-- ============================================================ -->
<!--                        中文版本                                -->
<!-- ============================================================ -->

## <a id="中文版本"></a> 🇨🇳 中文版本

# NexOS — 二阶引导加载 C++ 内核

一个功能完整的 x86 教学操作系统,支持 **BIOS** 和 **UEFI** 双引导路径。
Stage1 引导扇区 → Stage2 二阶引导 → 切换到 32 位保护模式 → 跳转到 C++ 内核。

## 内核特性

- 滚动回看终端 + 剪贴板(鼠标选择/中键粘贴/Ctrl+C/V)
- PS/2 键盘(Set 1)+ 鼠标(Intellimouse 滚轮)驱动
- PowerShell 风格 shell(`PS user@minios /path>`),48+ 命令
- **用户系统**:登录/多用户/密码哈希(影子文件持久化)
- **权限系统**:9 位 rwxrwxrwx,`chmod`/`stat`,文件命令自动鉴权
- **sudo**:验证密码后临时提权执行
- 双文件系统:MKFS(自制可写,支持**独立数据盘持久化**) + SFS(预构建只读)
- `.sh` 脚本执行(`run`/`runfs`)
- **GUI 桌面**(Win11 Portal 风格,按需启动,BGA fallback 兼容任意显卡)
- **中文支持**:GB2312 16×16 点阵字库(387 字),GUI 中英文混排渲染
- **拼音输入法(IME)**:GUI Terminal 内输入拼音 → 数字选字 → UTF-8 汉字
- 网络:NE2000 驱动 + HTTP 服务器
- AI 引擎(Markov + GPT 风格文本生成)
- 32↔64 位内核切换

## 目录结构

| 文件 | 说明 |
|------|------|
| `boot.asm` | Stage1,512 字节引导扇区,INT 13h 扩展读加载 Stage2 |
| `stage2.asm` | Stage2,加载内核、开 A20、建 GDT、切入 32 位保护模式 |
| `entry.asm` | 内核入口桩(32 位),设栈后调用 `kmain` |
| `kernel.cpp` | 32 位内核:终端 + 输入 + ATA + shell + 文件系统 + 用户/权限/sudo |
| `entry64.asm` | 64 位内核入口桩(长模式) |
| `kernel64.cpp` | 64 位内核(长模式变体) |
| `switch32to64.asm` | 32 → 64 位模式切换(`switch64` 命令) |
| `switch64to32.asm` | 64 → 32 位模式切换(返回命令行) |
| `gui.cpp` | GUI 桌面 + 中文渲染 + 拼音 IME |
| `winloader.cpp` | Windows 可执行文件加载器(`run xxx.exe/.bat/.ps1`) |
| `net.cpp` | NE2000 网卡驱动 + HTTP 服务器 |
| `ai_engine.cpp` | AI 文本生成引擎(Markov + GPT) |
| `linker.ld` / `linker64.ld` | 32/64 位内核链接脚本,入口 `_start` @ `0x10000` / `0x100000` |
| `uefi/bootuefi.c` | UEFI 引导程序(x86_64,gnu-efi):加载 kernel.bin、退出 Boot Services |
| `uefi/enter_kernel.S` | UEFI 模式切换:长模式 → 32 位兼容模式 → 跳转内核 |
| `tools/sfs_gen.py` | SFS 镜像生成器:打包 `sfs_files/` |
| `tools/gen_zfont.py` | 中文字库生成器:SimSun → GB2312 16×16 点阵(`zfont_data.h`) |
| `tools/gen_ime_dict.py` | 拼音字典生成器:pypinyin → `ime_dict.h` |
| `tools/make_data_vhd.py` | 用户数据盘生成器:预格式化 MKFS 的 8MB VHD |
| `sfs_files/` | SFS 源文件(`.sh` 脚本、`.txt` 文本) |
| `Makefile` | 构建:BIOS + UEFI + ISO + SFS + 数据盘 |
| `test.sh` / `test_uefi.sh` | BIOS/UEFI 无头自动化测试(串口 + 截图校验) |

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

Stage2 固定 16KiB,内核起始 LBA(33)确定。**MKFS 数据区默认写在独立的用户数据盘**(Secondary/Primary 上第一个 ATA 硬盘),启动介质(ISO/CD)可保持只读;没有数据盘时回退到启动盘(兼容旧镜像)。用户文件(含 `shadow` 用户库与 `permdb` 权限表)因此**跨重启持久化**。

UEFI 路径下,`BOOTX64.EFI` 从 ESP 读取 `kernel.bin` 复制到 `0x10000`,退出 Boot Services 后切到 32 位兼容模式跳转——与 BIOS 路径汇合。

## 依赖

### BIOS 构建

```bash
sudo apt install nasm g++ gcc-multilib make qemu-system-x86 python3
```

### UEFI 构建

```bash
sudo apt install gnu-efi ovmf mtools
```

### 中文字库 / 拼音字典(可选,构建时自动)

- `gen_zfont.py` 需要 Pillow + 一个中文字体(Windows SimSun / 文泉驿等)
- `gen_ime_dict.py` 需要 pypinyin(`pip install pypinyin`)

## 构建

```bash
make                 # 生成 BIOS 镜像 build/os.img (含 SFS)
make iso             # 生成 CD-ROM 镜像 build/os.iso (BIOS+UEFI 混合)
make uefi            # 生成 UEFI 镜像 build/os_uefi.img
make sfs             # 仅生成 SFS 镜像 build/sfs.img
make data-vhd        # 生成用户数据盘 build/data.vhd (8MB, 预格式化 MKFS)
make disasm          # 查看内核入口与反汇编,确认 _start 在 0x10000
```

SFS 镜像由 `tools/sfs_gen.py` 从 `sfs_files/` 打包;中文与拼音字典由 `gen_zfont.py` / `gen_ime_dict.py` 在 `gui.cpp` 编译前自动生成到项目根 (`zfont_data.h` / `ime_dict.h`)。

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

`make test` 通过 monitor `sendkey` 模拟输入,分 6 阶段导出 `0xB8000` 校验:help/echo 回显、方向键翻页、save/load 历史持久化、MKFS 增删改查、SFS 列目录/读文件/跑脚本。当前还含登录步骤(`root`/`admin`)。

`make uefi-test` 捕获串口验证:UEFI bootloader 加载内核 → 退出 Boot Services → x64→32 位兼容模式 → kmain → VGA 文本模式 → Hello world。OVMF 退出后 monitor 读 VGA 返回 `0xFFFFFFFF`,改用 `screendump` 截图统计非黑像素验证屏幕有内容。

## 命令行 shell

启动后进入**登录提示**,输入用户名/密码(预置 `root`/`admin`、`guest`/`guest`),成功后进入 shell,提示符 `PS user@minios /path>`。

### 基础命令

| 命令 | 作用 |
|------|------|
| `help` | 列出所有命令 |
| `echo <text>` | 打印文本 |
| `clear` / `cls` | 清屏并清空回看历史 |
| `about` | 系统信息 |
| `history` / `h` | 显示本次(及从磁盘载入的)命令历史 |
| `save` | 把命令历史写入磁盘(LBA 300) |
| `load` | 从磁盘读回命令历史 |

### MKFS 命令(自制可写文件系统)

| 命令 | 作用 |
|------|------|
| `mkfs` | 格式化 MKFS(数据盘或启动盘) |
| `ls` / `dir` | 列出 MKFS 上的文件 |
| `cat` / `type` | 打印 MKFS 文件内容 |
| `touch` / `ni` | 创建空文件 |
| `write <f>` | 逐行写入文本(空行结束,最大 8KB) |
| `rm` / `del` | 删除 MKFS 文件 |
| `copy` / `cp` | 复制文件 |
| `mkdir` / `md` | 创建目录 |
| `cd` / `sl` | 切换目录 |
| `pwd` / `gl` | 显示当前目录 |

### SFS / 分区 / 脚本

| 命令 | 作用 |
|------|------|
| `lsfs` | 列出 SFS 上的文件 |
| `catfs <f>` | 打印 SFS 文件内容 |
| `run <f>` | 执行 MKFS 上的 `.sh` 脚本 |
| `runfs <f>` | 执行 SFS 上的 `.sh` 脚本 |
| `part` / `mount` / `lsfat` / `fatinfo` | FAT32 分区管理与浏览 |

### 用户 / 权限 / sudo 命令

| 命令 | 作用 |
|------|------|
| `whoami` / `id` | 显示当前用户 / uid / gid |
| `users` | 列出所有用户 |
| `login <user>` | 切换用户(输密码) |
| `logout` | 退出登录,回到登录提示 |
| `su <user>` | 切换用户(默认 root,输密码) |
| `useradd <name> [pw]` | 添加用户(root only,默认密码 123456) |
| `deluser <name>` | 删除用户(root only,不能删 root/当前用户) |
| `passwd [user]` | 改密码(root 可改任何人;自己改需输旧密码) |
| `chmod <mode> <file>` | 改文件权限(如 600/644/755,owner 或 root) |
| `stat <file>` | 显示 owner / gid / mode(rwxrwxrwx + 八进制) |
| `sudo <cmd>` | 输密码提权后以 root 执行单条命令 |

### 内核 / GUI / 网络 / AI / 电源

| 命令 | 作用 |
|------|------|
| `gui` | 进入 GUI 桌面(BGA fallback,任意显卡可用) |
| `switch` / `switch64` | 切换到 64 位内核(长模式) |
| `netinfo` / `netstart` | 网络状态 / 启动 NE2000 + HTTP 服务器 |
| `ai` / `generate` / `agent` | AI 文本生成(初始模型 / 生成 / Agent) |
| `meminfo` / `memtest` / `pagetest` | 内存 / 页表诊断 |
| `run xxx.bat/.exe/.ps1` | 通过 winloader 在 GUI 中打开并执行 |
| `shutdown` / `reboot` | 关机 / 重启 |

## 用户系统

用户数据库持久化在 MKFS 根目录的 `shadow` 文件 (`name:uid:gid:group:hash\n`),密码用 **FNV-1a 32 位哈希**(盐=用户名, 8 位 hex)存储,不存明文。首次启动若无用户自动播种 `root/admin`(uid 0) 与 `guest/guest`(uid 1000)。`useradd`/`deluser`/`passwd`/`login` 都会更新 `shadow` 并写回数据盘。

## 权限系统

- 9 位标准 `rwxrwxrwx`,权限表持久化在 MKFS 根目录 `permdb` (`name:uid:gid:mode3`);`FileEntry.reserved` 冗余存低 8 位。
- 鉴权顺序:root(uid 0)放行 → owner 位 → group 位 → other 位。
- `cat`(r)、`rm`/`write`/`touch`(w)自动检查;新建文件/目录默认 0644/0755,归属创建者。
- `sudo <cmd>`:提示当前用户密码,验证通过后以 effective uid=0 执行,命令结束自动降权。

## 文件系统

### MKFS — 自制可写文件系统

```
LBA 512:     超级块   magic="MKFS", version, file_count, free_lba, total_sectors
LBA 513-528: 文件表   16 扇区, 每扇区 16 条目 = 最多 256 文件
LBA 529+:    数据区   启动盘 271 扇区 / 数据盘 15200 扇区, 顺序分配
```

文件条目(32 字节):`name[20] + size(4) + start_lba(4) + type(1) + parent(2) + reserved(1)`。

- `mkfs` 格式化:写魔数、清表、重置 `free_lba`;数据盘用大数据区。
- `write` 逐行输入(`>>` 提示符),空行结束保存;`create` 顺序分配扇区,同名覆盖;`mkdir`/`cd` 支持目录层级。
- **数据盘自动检测**:启动时扫描 ATA 硬盘(排除 ATAPI CD-ROM),找到即把 MKFS 写在那里 —— 重启后文件仍在,启动介质可只读。
- `shadow` / `permdb`(用户库 / 权限表)也存这里,随用户操作实时更新。

### SFS — 兼容只读文件系统

```
LBA 800:     超级块   magic="SFS", version, file_count, data_start
LBA 801-816: 目录     16 扇区, 最多 256 文件条目
LBA 817+:    数据区   按文件顺序排列, 每文件按扇区对齐
```

`sfs_files/` 中的文件在 `make` 时由 `tools/sfs_gen.py` 打包,格式与 MKFS 文件条目兼容,支持任意类型(`.sh`/`.txt` 等),文件名最长 23 字符。

当前包含:`hello.sh`(示例脚本)、`test.sh`(测试脚本)、`welcome.txt`。

### 脚本执行

`run`/`runfs` 逐行读取并执行 `.sh`:跳过空行与 `#` 注释,每条命令以黄色 `> ` 前缀回显后调用 `run_command`;脚本内命令不污染历史 (`g_in_script` 标志)。

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
- Portal 桌面:搜索栏 + 8 个彩色应用图标(Control/Files/Tasks/Memory/Terminal/Browser/Calc/About)+ 三卡片 + Home/Apps/System/Tools
- 可拖动窗口:Terminal(支持中文 + IME)/浏览器/计算器/任务管理器/控制面板/文件浏览器/About
- `run xxx.bat/.exe/.ps1` 在 GUI 中打开并执行,输出显示在窗口内

**VBE 适配**:BIOS 下 QEMU Cirrus/VMSVGA 没有经典 VBE BIOS 时,内核自动探测 **BGA ports(0x1CE/0x1CF)** 合成 VBE info(1024×768×32, LFB 0xFD000000),因此任何 QEMU/VirtualBox 显卡都能进 GUI;UEFI 路径走 GOP + shadow framebuffer。

## 中文支持

- GB2312 16×16 点阵字库(387 个 UI 常用字),由 `tools/gen_zfont.py` 从 SimSun 渲染(22px → LANCZOS 缩放到 16×16 → 阈值二值化)嵌入内核。
- `draw_text_utf8` / `draw_text_utf8_transparent`:UTF-8 3 字节 → Unicode → 二分查找字库 → 16×16 blit;ASCII 走原 8×16 字体,中英文混排。
- 顶栏 `欢迎使用`、Terminal 输出/输入均支持中文。

## 拼音输入法(IME)

- `tools/gen_ime_dict.py`:pypinyin 把 387 字转拼音,输出 `ime_dict.h` (按拼音排序,前缀匹配)。
- GUI Terminal 内直接可用:

| 按键 | 行为 |
|------|------|
| `a-z` | 进入拼音状态,显示候选条(紫拼音+黄编号+白字) |
| `1-9` | 选第 N 个候选,UTF-8 写入输入行 |
| `空格` | 选第 1 个候选 |
| `退格` | 删最后一个拼音字符 |
| `ESC` | 取消 IME(不退出 GUI) |

## 键盘 / 鼠标 / 终端

- **键盘**:`0x64` 状态口,`0x60` 读 Set 1 扫描码;支持 Backspace/Enter/Tab/Shift/CapsLock/方向键/Home/End/PgUp/PgDn/Ctrl+C(复制或中止)/Ctrl+V(粘贴)/Ctrl+L(聚焦)/Ctrl+↑↓(剪贴板历史)。
- **鼠标**:PS/2 Intellimouse(4 字节包,200/100/80 采样序列启用滚轮);左键拖选复制,右键聚焦输入,滚轮翻页。
- **终端**:200 行环形回看缓冲,25 行窗口;`m_view` 视口 + `m_at_bottom` 贴底;翻页不破坏历史;光标用 0x3D4/0x3D5 定位,离开底部自动隐藏;任意输入自动 snap 回底部;支持 framebuffer console 模式(BGA/OVMF)。

## BIOS 引导流程

1. **Stage1**(`0x7C00`,16 位实模式):保存启动盘号,INT 13h AH=42h 从 LBA 1 读 32 扇区 Stage2 到 `0x8000`,远跳转。
2. **Stage2**(`0x8000`,16 位实模式):从 LBA 33 读内核到 `0x10000`;端口 `0x92` 开 A20;加载 GDT;置 CR0.PE 切保护模式;远跳转 `CODE_SEG:init_pm` 刷流水线、装段寄存器,跳到 `0x10000`。
3. **内核**(`0x10000`,32 位 PM):`entry.asm` 设栈 → `kmain`:硬件探测 → VBE 探测(BGA fallback)→ 终端/键盘/鼠标/文件系统 → 用户库加载 → 强制登录 → shell 循环。

## UEFI 引导流程

1. **OVMF**:扫描 ESP 启动 `/EFI/BOOT/BOOTX64.EFI`(CPU 在长模式)。
2. **bootuefi.c**(64 位):LoadedImage 协议 → SimpleFileSystem 读 `kernel.bin` → `AllocatePages(AllocateAddress, 0x10000)` 复制内核 → 取内存映射 → `ExitBootServices`。
3. **enter_kernel.S**:RIP 相对 LEA 建 GDTR → LGDT → LRETQ 远返回 32 位**兼容模式**段(长模式内执行 32 位指令,UEFI 恒等映射页表保持有效)→ 装平坦段、设栈 `0x90000` → 远跳 `0x10000`。
4. **内核**(32 位兼容模式):与 BIOS 路径相同;额外调 `vga_set_text_mode()` 把 VGA 从 OVMF 图形模式切回文本模式 3。

### UEFI 调试

串口(0x3F8)输出标记:`[UEFI]`(bootloader 各阶段)、`E`(enter_kernel 入口)、`G`(GDT 已加载)、`3`(进入 32 位兼容模式)、`S`(内核 `_start`)、`[K1]`~`[K6]`(kmain 各阶段)、`[K32]`(进入 GUI)。

### 设计决策:兼容模式 vs 完整模式切换

UEFI 启动后 CPU 在长模式,而内核是 32 位代码。**兼容模式方案**用 `L=0, D=1` 段在长模式内执行 32 位指令,UEFI 页表保持有效,所有 I/O/VGA/内存行为与 32 位保护模式一致,避免完整切换(关分页→关 PAE→关 LME)的三重错误风险。CR0/CR4/EFER 仍保持长模式配置,对内核透明。

---

# NexOS 开源协议

**NexOS 开源协议（MiniOS Open Source License）**  
版权所有 (c) 2026 MiniOS 项目开发者（以下简称“项目方”）

允许并鼓励任何个人或组织出于学习、研究、教学、个人兴趣或非商业目的，使用、复制、修改、合并、发布、分发本软件及其衍生版本。

## 一、定义

1.1 “软件”指本协议所约束的源代码、二进制文件、文档及相关资源。

1.2 “衍生版本”指在原始软件基础上进行修改、扩展、移植后形成的新版本。

## 二、允许的行为

2.1 **学习与研究**  
任何人有权查看、分析、运行本软件，用于学习操作系统原理、编程技术或进行科学研究。

2.2 **修改与扩展**  
任何人有权对本软件进行修改、优化、功能增强，并创建衍生版本。

2.3 **分发衍生版本**  
任何人有权在遵守本协议的前提下，分发其基于本软件创建的衍生版本。分发时须清晰标注衍生版本的修改范围，并保留本协议及版权声明。

2.4 **内部使用**  
企业或组织内部出于教育、培训或内部技术验证目的使用本软件，不受商业使用限制。

## 三、禁止的行为

3.1 **商业使用**  
未经项目方书面明确许可，任何人不得将本软件或其衍生版本用于任何商业目的，包括但不限于：

a) 直接或间接通过本软件收费、盈利或获取商业利益；  
b) 将本软件作为商品或服务的一部分进行销售；  
c) 将本软件集成到用于出售或出租的软件产品中；  
d) 使用本软件为商业客户提供技术服务、外包或咨询服务并以此获利。

3.2 **盗用或剽窃**  
任何人不得将本软件或其衍生版本声称为自己原创，不得移除或篡改版权声明、作者信息及本协议内容。

3.3 **恶意使用**  
任何人不得使用本软件从事任何违法、危害网络安全、侵犯他人权益的活动。

## 四、衍生版本要求

4.1 任何衍生版本必须在显著位置保留本协议全文或提供明确的本协议引用链接。

4.2 衍生版本的分发者须明确说明其对原始软件的修改内容，并确保衍生版本不误导他人认为其与原始项目方有直接关联。

4.3 衍生版本同样适用本协议的条款，特别是禁止商业使用的限制。

## 五、免责声明

本软件以“现状”（AS IS）提供，项目方不对其适销性、特定用途适用性、安全性或任何其他方面提供任何明示或暗示的保证。项目方不对因使用本软件而导致的任何直接、间接、特殊、偶然或结果性损失承担责任。

## 六、终止

如违反本协议任何条款，本协议授予的权利将自动终止。违反者须立即停止使用并销毁所有副本。

## 七、最终解释权

本协议最终解释权归项目方所有。项目方保留根据实际情况调整本协议条款的权利，调整后的条款将通过适当方式通知用户。

**版本：1.0**  
**生效日期：2026-08-11**

---

<p align="right"><a href="#nexos--二阶引导加载-c-内核">⬆ 返回顶部</a></p>

<!-- ============================================================ -->
<!--                        英文版本                                -->
<!-- ============================================================ -->

---

## <a id="english-version"></a> 🇬🇧 English Version

# NexOS — Two-Stage Bootloader C++ Kernel

A full-featured x86 educational operating system with dual BIOS/UEFI boot paths.
Boot flow: Stage1 boot sector → Stage2 loader → switch to 32-bit protected mode → jump to C++ kernel.

## Features

- Scrollback terminal + clipboard (mouse select / middle-click paste / Ctrl+C/V)
- PS/2 keyboard (Set 1) + mouse (Intellimouse w/ scroll wheel) drivers
- PowerShell-style shell (`PS user@minios /path>`), 48+ commands
- **User system**: login / multi-user / password hashing (shadow file persistence)
- **Permission system**: 9-bit rwxrwxrwx, `chmod`/`stat`, automatic permission checks on file commands
- **sudo**: temporary privilege elevation with password verification
- Dual file systems: MKFS (writable, with **dedicated data disk persistence**) + SFS (pre-built read-only)
- `.sh` script execution (`run`/`runfs`)
- **GUI desktop** (Win11 Portal style, on-demand launch, BGA fallback for any GPU)
- **Chinese support**: GB2312 16×16 bitmap font (387 glyphs), mixed Chinese/English rendering in GUI
- **Pinyin IME**: type pinyin in GUI Terminal → select by number → UTF-8 Chinese characters
- Networking: NE2000 driver + HTTP server
- AI engine (Markov + GPT-style text generation)
- 32↔64 bit kernel switching

## Directory Structure

| File | Description |
|------|-------------|
| `boot.asm` | Stage1, 512-byte boot sector, INT 13h extended read to load Stage2 |
| `stage2.asm` | Stage2, loads kernel, enables A20, sets up GDT, enters 32-bit protected mode |
| `entry.asm` | Kernel entry stub (32-bit), sets up stack and calls `kmain` |
| `kernel.cpp` | 32-bit kernel: terminal + input + ATA + shell + file systems + user/perm/sudo |
| `entry64.asm` | 64-bit kernel entry stub (long mode) |
| `kernel64.cpp` | 64-bit kernel (long mode variant) |
| `switch32to64.asm` | 32 → 64 bit mode switch (`switch64` command) |
| `switch64to32.asm` | 64 → 32 bit mode switch (return to command line) |
| `gui.cpp` | GUI desktop + Chinese rendering + Pinyin IME |
| `winloader.cpp` | Windows executable loader (`run xxx.exe/.bat/.ps1`) |
| `net.cpp` | NE2000 NIC driver + HTTP server |
| `ai_engine.cpp` | AI text generation engine (Markov + GPT) |
| `linker.ld` / `linker64.ld` | 32/64-bit kernel linker scripts, entry `_start` @ `0x10000` / `0x100000` |
| `uefi/bootuefi.c` | UEFI bootloader (x86_64, gnu-efi): loads kernel.bin, exits Boot Services |
| `uefi/enter_kernel.S` | UEFI mode switch: long mode → 32-bit compatibility mode → jump to kernel |
| `tools/sfs_gen.py` | SFS image generator: packs `sfs_files/` |
| `tools/gen_zfont.py` | Chinese font generator: SimSun → GB2312 16×16 bitmap (`zfont_data.h`) |
| `tools/gen_ime_dict.py` | Pinyin dictionary generator: pypinyin → `ime_dict.h` |
| `tools/make_data_vhd.py` | User data disk generator: pre-formatted MKFS 8MB VHD |
| `sfs_files/` | SFS source files (`.sh` scripts, `.txt` text files) |
| `Makefile` | Build: BIOS + UEFI + ISO + SFS + data disk |
| `test.sh` / `test_uefi.sh` | Headless automated tests for BIOS/UEFI (serial + screenshot checks) |

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
