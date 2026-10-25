# K64 BLIT CONFLICT — 64 位内核启动失败的根因

状态：**未解决**，已移交 `kernel-architect`
记录时间：2026-10-25
记录人：Lead

---

## 1. 症状

在 32 位文本 shell 里执行 `switch`（或 `ask64`）后，串口输出：

```
PS root@NexOS /> switch
[SHELL] $ switch

Switching to 64-bit kernel...
[K32] Switching to 64-bit kernel...
Loading kernel64.bin from LBA 2048...
[K32] staging at 10005C80, sectors=00001000
[K32] stage[0..3]=03F8BA66 (expect 03F8BA66 = '66 BA F8 03' entry64)
[K32] K64 image staged; entering long mode
SdMPACELGG`                     <-- 垃圾输出，之后无任何内容
```

**`[K64-1] kmain64 entered` 从未出现。**

`entry64.asm` 的调试标记序列应为 `E F G H I J K L`（`DBG 0x45`..`DBG 0x4C`）。
实际输出 `SdMPACELGG` 与之完全不符 —— 说明 CPU 在 long mode 里执行的**不是** entry64 的代码。

## 2. 已排除的原因（都已修复并验证）

以下四个问题**确实存在且已修复**，但修完症状不变 —— 它们不是最终根因：

| # | 问题 | 修复 |
|---|------|------|
| 1 | `os_v2.img` 被并发构建截断成 9,373,648 B（应为 24,080,384） | 重新构建；并在 `tools/fastverify.sh` 加截断检查 |
| 2 | `make textboot` 链接失败（缺 `$(PLUGIN_OBJS)`/`$(DRIVER_FW_OBJS)` 等） | 补全链接行；`$^` 改为显式对象列表（`linker.ld` 已由 `-T` 传入，重复会报错） |
| 3 | `switch` 的 staging 地址被拒：上限写死 `0x02000000`，而堆已搬到 `HEAP_START=0x10000000` | 改用 `IDENT_TOP = 0x18000000`（`HEAP_END`，即 identity map 上界），见 `kernel.cpp:7470` |
| 4 | 64 位栈硬编码 `0x1F0000`，而镜像已长到 `0x1CA8B8`，栈落在镜像内部 | 改用 `0x1828000`（= `linker.ld` 的 `__lm_stack_top`），见 `.attic64/entry64.asm` |

修完 #3/#4 后，staging 校验通过（`stage[0..3]=03F8BA66` 正确），blit 长度也正确
（`mov $0x32a2e,%ecx` = 207,406 dwords = 829,624 B = 精确镜像大小），
但 long mode 仍然执行垃圾。

## 3. 根因假设（高置信度）：blit 覆盖 32 位内核的 .bss

`switch32to64.asm` 把整个 `kernel64.bin` blit 到 **`KERNEL64_ENTRY = 0x100000`**：

```nasm
%define KERNEL64_ENTRY  0x100000
%define KERNEL64_DWORDS ((KERNEL64_BYTES + 3) / 4)   ; 构建期注入真实镜像大小
...
    mov edi, KERNEL64_ENTRY
    mov ecx, KERNEL64_DWORDS
    cld
    rep movsd
```

### 实测地址（`nm build/kernel.elf`）

| 符号 | 地址 |
|------|------|
| `__bss_start` | `0x00120000` |
| `__bss_end` | `0x009A6204` |
| `__lmboot_start` | `0x01800000` |
| `__lm_stack_top` | `0x01828000` |
| `switch_to_64bit` | `0x000100A0` |

### blit 覆盖范围

```
目标   : 0x100000 .. 0x100000 + 829624 = 0x1CA8B8
.bss   : 0x120000 .. 0x9A6204      (9,069,060 B)
重叠   : 0x100000 .. 0x9A6204      <-- blit 破坏了 32 位内核 9 MB 的 .bss
```

### 为什么这是致命的

`switch32to64.asm` 内的历史注释写着：

> ```
> The blit also trashes .bss @ 0x120000..0x180000, which is deliberate:
> we never return to C++ from here.  switch_to_64bit itself lives at
> 0x10060 (< 1 MiB), so it is NOT overwritten.
> ```

**这个假设已经过期两处：**

1. 注释认为 `.bss` 只到 `0x180000`（1.5 MiB）；实测已到 **`0x9A6204`（9.6 MiB）** ——
   内核在 .bss 里塞进了 AI/网络/GUI/驱动框架等大量全局状态。
2. 注释认为 blit 只覆盖 `.bss` 的"无关部分"。但 `.bss` 现在延伸进
   **`0x100000..0x9A6204`**，而 `switch_to_64bit` **在 blit 之后仍需继续执行**：
   它要建立 long-mode 页表、装载 `gdt64`、设置 `esp` 到 `__lm_stack_top`，
   再远跳到 64 位入口。

代码段 `switch_to_64bit @ 0x100A0` 本身在 1 MiB **以下**，没被覆盖；
但**它依赖的数据**（页表存储 `g_page_directory_store`、`gdt64` 描述符等，
均在 `.bss`/`.data` 中，地址落在 1 MiB 以上）会被 blit 抹掉。

> 注：`switch32to64.asm` 的注释还提到一个历史事故 —— 分页开启时写
> `0x100000..0x180000` 会破坏 `g_page_directory_store @ 0x128000`，
> 所以 blit 必须在**关分页**下进行。该问题当时已用"关分页"绕过，
> 但**"blit 会破坏 .bss"这个更根本的问题从未被解决**，只是随着内核变大而恶化。

## 4. 需要 kernel-architect 决策的方向

两条路线（或其它更优方案）：

### 路线 A：把 64 位镜像搬到不与 32 位内核重叠的地址
- 修改 `KERNEL64_ENTRY`（`switch32to64.asm`）
- 同步 `linker64.ld` 的 `. = 0x100000`
- 同步 `.attic64/entry64.asm` 中任何硬编码的基址假设
- 同步 `kernel.cpp` 的 `KERNEL64_LBA_DISK`/`KERNEL64_SECTORS` 语义与
  `uefi/bootuefi.c` 的预载地址（UEFI 路径把 kernel64 预载到某处，`0x7000` handoff）
- 需选一个 **> `__bss_end`(0x9A6204) 且 < identity map 上界** 的地址，
  且不与 `.lmboot`(0x1800000..0x1828000)、RAM-SFS(0x1400000+0x400000)、
  32 位 heap 冲突
- **风险**：牵动 BIOS + UEFI 两条启动路径

### 路线 B：blit 前把必需数据迁到安全区
- 把 `g_page_directory_store`、`gdt64` 等 blit 后仍需的数据复制到 1 MiB 以下，
  或到 `.lmboot` 区
- 或改为**分块 blit**：只覆盖 1 MiB 以下不冲突的部分，其余用别的方式加载
- **风险**：需要精确枚举"blit 后仍被使用"的所有全局

### 附加要求
- 修复后必须用 `tools/fastverify.sh` 验证（**2.8 秒**一轮，不要再手工等 QEMU）
- 验收：`[K64-1] kmain64 entered` 出现，且 64 位侧驱动扫描运行
- 32 位基线不得回退：`scan complete: 1 loaded` + `[GUI] Entered GUI mode`

## 5. 复现步骤（已脚本化）

```bash
# 1. 构建文本模式镜像（唯一能拿到 shell 的镜像；UEFI/BIOS 默认走 32 位 GUI）
make textboot

# 2. 启动并登录（root/admin），然后执行 switch
#    见本文档附带的 _sw*.sh 脚本；或手工：
qemu-system-i386 -m 512 -drive file=build/os_textboot.img,format=raw,if=ide \
  -display none -serial file:/tmp/s.txt -no-reboot \
  -monitor unix:/tmp/m.sock,server,nowait &

# 3. 用 monitor sendkey 输入：root<ret> admin<ret> switch<ret>

# 4. 检查
grep -aE 'kmain64|K64-1|entering long mode' /tmp/s.txt
```

## 6. 重要教训（写入本仓库备忘）

1. **64 位路径此前从未被真正验证过。** UEFI 镜像默认启动 **32 位**内核
   （`[K0] kmain entered` / `[K32] Entering Win11 GUI mode`），
   64 位只能通过 32 位终端里的 `switch`/`ask64` 进入。
   此前所有"64 位加载 fontvec 成功"的结论，实际都是 **32 位输出**。
2. **任何"启动卡死"结论，必须先验证镜像完整性。**
   `make -q` 只比时间戳，不检查内容；一个被中断的 `cat ... > img`
   会留下短文件而被判定为"最新"，之后所有基于它的分析都是假的。
3. **常量会随体积增长而失效。** 本问题由三个"写死的地址/大小"共同造成：
   staging 上限 `0x02000000`、64 位栈 `0x1F0000`、blit 覆盖假设 `0x180000`。
   修复时应尽量改成**由构建期注入或由链接脚本导出**，避免再次静默失效。

---

## 7. 【重要更正】§3 的判断已被证伪 —— 真正的根因是另外两个

> 本节由 `kernel-architect` 在 task-13 中实测得出，并**用二分法直接证伪了本文档 §3**。
> §3 的「blit 覆盖 .bss」结论**不成立**，保留在文档中仅作为排查过程记录。

### 证伪方法

临时把 BIOS 路径的 blit 改成 `xor ecx,ecx`（一个字节都不拷贝），重建后串口输出
**与原来完全一致**（`SdMPACELGG` + `0x60 0x10 0x10 0x10 0x10 0x60`）。
若 blit 是根因，跳过它必然产生不同输出。故 `.bss`/`.lmboot` 被覆盖与本次失败**无关**。

另更正 §3 的一处笔误：`0x100000..0x9A6204` 是 `.bss` 的**总大小**（9 MB），
blit 与 `.bss` 的实际重叠是 **698,552 B ≈ 682 KiB**，与 `.lmboot` 重叠 **305,336 B ≈ 298 KiB**。

### 真正的根因 A：`jmp 0x08:label` 的绝对偏移被算错 2 字节（仅 BIOS 路径）

```
nm kernel_textboot.elf -> long_mode_entry = 0x10344
BIOS 路径汇编成   ea 46 03 01 00 08 00  = ljmp 0x08:0x10346   <- 差 2
compat 路径汇编成 ea 44 03 01 00 08 00  = ljmp 0x08:0x10344   <- 正确
```

CPU 落在 `0x10346`，正好是 `mov dx,0x3FD`（`66 BA FD 03`）的第 3 个字节，
从 `fd 03` 开始执行（`std` / `add esp,ebp`…），于是输出垃圾、`kmain64` 永不出现。
**同一行源码出现两次，NASM 给出两个不同结果** —— 是「写死编码」陷阱的变种。

**修法**（不改地址）：两处都改为基于重定位的 far transfer：
```nasm
push dword 0x08
push dword long_mode_entry
retf
```
目标地址成为普通 32 位绝对立即数（`R_386_32`），由链接器精确修补，不可能再差 2 字节。

### 真正的根因 B（决定性）：`qemu-system-i386` 根本没有 long mode

QEMU `-d int` 抓到的第一次异常就是答案：
```
v=0e e=0002 IP=0008:000101d6 SP=0010:0180fd8c CR2=0180fd88
EFER=0000000000000000      <- LME 从未生效
CS =0008 ... 00cf9a00 CS32  <- 仍在 32 位兼容段
CR0=80000033 CR4=00000620   <- PG=1, PAE=1
CR3=01810000                <- 指向我们的「PML4」
然后 v=08(#DF) -> 三重点
```

`qemu-system-i386` 是**32 位专用 target**，CPU 模型不带 LM 位，`wrmsr(EFER.LME)` 被静默丢弃。
CPU 于是以「legacy 32-bit + PAE」运行，把 64 位 PML4 当作 PAE 的 PDPT 解析：
`PDPTE[0]=0x1811003` → 页目录在 `0x1811000`，其中 8 个 `0x83`（本意 1 GiB 页）被当成 PDE，
只有 `PDE[0]` 有 `PS=1`，`PDE[12]`（24-26 MiB）为 0 → push 到 `.lmboot` 栈地址
`0x180fd88` 时写时缺页（`e=0x0002`）→ `#DF` → 三重点。

**这是比地址问题更根本的教训：验证命令本身用的是不能进 long mode 的模拟器。**
历史上所有 `qemu-system-i386` 运行，无论代码对错，**都不可能成功**。

### 为什么 A/B 两条路线都不用走

```
blit 目标       : 0x100000 .. 0x1CA8B8   (829,624 B)
与 .bss 重叠    : 682 KiB
与 .lmboot 重叠 : 298 KiB
```

但**目前完全无害**：blit 之后 32 位内核只再做三件事 —— 建页表、`lgdt`、跳 64 位。
- 页表写在 `.lmboot`（`0x1810000` 起），是 blit **之后**才写的；
- 64 位栈 `0x1828000` 也是 blit **之后**才赋值使用；
- `gdt64` 在 `0x9a660`（< 1 MiB），根本不在 blit 范围内。

`.bss` 被践踏的部分在跳转 64 位后永不访问。
所以**改地址（路线 A）要同时改 `linker64.ld`/`entry64.asm`/UEFI 预载地址，收益为零、风险不小**；
**迁移数据（路线 B）要精确枚举「blit 后仍使用」的全局，而这个集合现在是空集**。

### 最终采用方案

1. 修掉真正的编码错误（`push/retf` 替代 `jmp seg:label`）—— 两处；
2. `kernel.cpp` `do_switch64()` 开头加 **long mode 可用性闸门**：
   `CPUID.80000001:EDX[29]`(LM) 不存在时打印
   `[K32] 64-bit refused: CPUID.80000001:EDX.LM=0 (no long mode; use qemu-system-x86_64)`
   并正常返回，**不再三重点**（符合「失败只 LOG 不 Panic」契约）；
3. `Makefile` 新增 `verify64` 目标（用 `qemu-system-x86_64`）。
4. `tools/fastboot.sh` / `tools/fastverify.sh`：QEMU 可执行文件改为 `${QEMU_BIN:-qemu-system-i386}`，
   64 位验证用 `QEMU_BIN=qemu-system-x86_64`。

**地址一律未动**：`KERNEL64_ENTRY` / `linker64.ld` / `entry64.asm` / `uefi/bootuefi.c` / `linker.ld` 均保持原样。

### 保留的隐患（记录备查）

blit 长度受 2 MiB 暂存窗口约束（`0x100000 + 0x200000 = 0x300000`），`kernel64.bin` 现为 `0xCA8B8`。
`Makefile` 的 K64 GUARD 已守住这个窗口，**暂时安全**。
若将来真的涨到 2 MiB，再按路线 A 搬到 `0x2000000` 一类空档。

### 实测结果（task-13）

| 测试 | 结果 |
|---|---|
| 64 位（`qemu-system-x86_64` + 登录 + `switch`） | `SdMPACELG6aefgbcdHIJKL[K64-1] kmain64 entered` + `[K64-IDT] installed` + `[K64-FB] ...` + `[DRIVER] scan: 3 .drv` + `scan complete: 1 loaded` ✅ |
| 32 位基线（`qemu-system-i386` GUI） | `scan complete: 1 loaded` + `hot-loaded 1` + `[GUI] Entered GUI mode` ✅ |
| 32 位专用模拟器下 `switch` | `[K32] 64-bit refused: CPUID.80000001:EDX.LM=0` 优雅返回，**无三重点** ✅ |

其中 `6aefgbcd` 正是 `entry64` 的 64 位标记序列 —— **此前从未出现过**。
