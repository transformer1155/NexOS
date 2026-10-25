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
