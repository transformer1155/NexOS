# MiniOS — 二阶引导加载 C++ 内核

一个功能完整的 x86 教学操作系统,支持 **BIOS** 和 **UEFI** 双引导路径。
Stage1 引导扇区 → Stage2 二阶引导 → 切换到 32 位保护模式 → 跳转到 C++ 内核。

内核特性:

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

| 文件                    | 说明                                                          |
|-------------------------|---------------------------------------------------------------|
| `boot.asm`              | Stage1,512 字节引导扇区,INT 13h 扩展读加载 Stage2             |
| `stage2.asm`            | Stage2,加载内核、开 A20、建 GDT、切入 32 位保护模式           |
| `entry.asm`             | 内核入口桩(32 位),设栈后调用 `kmain`                        |
| `kernel.cpp`            | 32 位内核:终端 + 输入 + ATA + shell + 文件系统 + 用户/权限/sudo |
| `entry64.asm`           | 64 位内核入口桩(长模式)                                       |
| `kernel64.cpp`          | 64 位内核(长模式变体)                                         |
| `switch32to64.asm`      | 32 → 64 位模式切换(`switch64` 命令)                           |
| `switch64to32.asm`      | 64 → 32 位模式切换(返回命令行)                                |
| `gui.cpp`               | GUI 桌面 + 中文渲染 + 拼音 IME                                |
| `winloader.cpp`         | Windows 可执行文件加载器(`run xxx.exe/.bat/.ps1`)             |
| `net.cpp`               | NE2000 网卡驱动 + HTTP 服务器                                 |
| `ai_engine.cpp`         | AI 文本生成引擎(Markov + GPT)                                |
| `linker.ld` / `linker64.ld` | 32/64 位内核链接脚本,入口 `_start` @ `0x10000` / `0x100000` |
| `uefi/bootuefi.c`       | UEFI 引导程序(x86_64,gnu-efi):加载 kernel.bin、退出 Boot Services |
| `uefi/enter_kernel.S`   | UEFI 模式切换:长模式 → 32 位兼容模式 → 跳转内核              |
| `tools/sfs_gen.py`      | SFS 镜像生成器:打包 `sfs_files/`                              |
| `tools/gen_zfont.py`    | 中文字库生成器:SimSun → GB2312 16×16 点阵(`zfont_data.h`)    |
| `tools/gen_ime_dict.py` | 拼音字典生成器:pypinyin → `ime_dict.h`                        |
| `tools/make_data_vhd.py`| 用户数据盘生成器:预格式化 MKFS 的 8MB VHD                     |
| `sfs_files/`            | SFS 源文件(`.sh` 脚本、`.txt` 文本)                           |
| `Makefile`              | 构建:BIOS + UEFI + ISO + SFS + 数据盘                         |
| `test.sh` / `test_uefi.sh` | BIOS/UEFI 无头自动化测试(串口 + 截图校验)                  |

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

Stage2 固定 16KiB,内核起始 LBA(33)确定。**MKFS 数据区默认写在独立的
用户数据盘**(Secondary/Primary 上第一个 ATA 硬盘),启动介质(ISO/CD)可
保持只读;没有数据盘时回退到启动盘(兼容旧镜像)。用户文件(含 `shadow`
用户库与 `permdb` 权限表)因此**跨重启持久化**。

UEFI 路径下,`BOOTX64.EFI` 从 ESP 读取 `kernel.bin` 复制到 `0x10000`,
退出 Boot Services 后切到 32 位兼容模式跳转——与 BIOS 路径汇合。

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

SFS 镜像由 `tools/sfs_gen.py` 从 `sfs_files/` 打包;中文与拼音字典由
`gen_zfont.py` / `gen_ime_dict.py` 在 `gui.cpp` 编译前自动生成到项目根
(`zfont_data.h` / `ime_dict.h`)。

## 运行 / 测试

### BIOS

```bash
make run              # QEMU 窗口(图形环境),可敲键盘/滚鼠标
make test             # 无头自动测试:shell + 翻页 + save/load + 文件系统 + 脚本
make bios-run         # BIOS 镜像 (os.img)
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
| `meminfo` / `memtest` / `pagetest` | 内存 / 页表诊断            |
| `run xxx.bat/.exe/.ps1` | 通过 winloader 在 GUI 中打开并执行       |
| `shutdown` / `reboot` | 关机 / 重启                               |

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
