# task-36 审计：Sfs::rd → ata_read_sector 的实参链（只读）

**性质**：纯静态 + 反汇编核查，**未改任何文件**。针对 **32 位 `build/kernel.bin`**（sha16 `a98fbb90`，588,604 B）。

## 0. 结论

| 项 | 结论 |
|---|---|
| (1) `lba = fent.start_lba + i + delta` | **成立**（源码 L2789 `data_lba(fent.start_lba + i)`，`data_lba` = `lba + delta`） |
| (2) tail call 的寄存器约定 | **成立**（`Sfs::rd` 末 `jmp 0x1169b`；在入口 `mov %eax,%ebx` 取 lba、`mov %edx,-0x24(%ebp)` 取 buf） |
| (3) `dev=0` ⇒ base `0x1F0`，7 个 outb 全在 `0x1F0..0x1F7` | **成立** |
| (4) `kernel.cpp:1131` 的硬编码 `0xE0` 是否缺陷 | **不是缺陷**（`ata_read_sector_dev` 没有 `drv` 形参，且 `dev=0` 就必须是 `0xE0`） |
| (5) webemu 是否丢 LBA 高 4 位 | **是，真实缺陷**，但**当前无害** |

## 1. 关于地址漂移（先说明方法）

**你给的地址已经漂移**：

    0x7680x 现在属于 Mkfs::mkdir（实测：`00076800 <_ZN4Mkfs5mkdirEPKc+0x1a>`）
    真正的 Sfs::rd 在 **0x768b2**；Sfs::read 在 **0x76bf0**（按 `nm` 符号定位）

⇒ 所以本报告**全部按符号名定位**，不按行号/地址（这两者在共享树里都会漂）。

## 2. 三项核查的实际证据

### (1) 数据扇区地址 `start_lba + i + delta`

源码：`kernel.cpp:2789`

    rd(data_lba(fent.start_lba + (uint32_t)i), (uint16_t*)g_fsbuf);

`data_lba()`（L2674 附近）：`return lba + (uint32_t)delta;` ⇒ 你描述的 `start_lba + i + delta` **成立**。

### (2) tail call 的两个实参

`Sfs::rd` 的 ATA 分支（0x768dd..0x768e2）：

    768dd: mov 0x10(%ebp),%edx     ; edx = buf  (rd 的第 3 个参数)
    768e0: pop %ebx
    768e1: pop %ebp
    768e2: jmp 1169b <_ZL15ata_read_sectorjPt>

被调者入口（0x1169b）：

    116a3: mov %eax,%ebx           ; ebx = lba   (rd 的第 2 个参数，仍是 EAX)
    116a8: mov %edx,-0x24(%ebp)    ; 保存 buf
    116b2: shr $0x18,%eax          ; lba>>24 -> -0x1c(%ebp)

⇒ **寄存器约定成立**：EAX=lba、EDX=buf，与 i386 regparm/fastcall 的第 1/2 参数一致；
   `rd` 在 jmp 前把 ebp 弹掉（自己的帧已拆），因此被调者看到的是**调用者的帧**——这正是 tail call 的正确形态。

### (3) `dev=0` ⇒ base 0x1F0，7 个 outb 全在 0x1F0..0x1F7

    1120: uint16_t base = dev ? 0x170 : 0x1F0;        // dev=0 -> 0x1F0
    1131: outb(base+6, ...)   1132: outb(base+1, ...)   1133: outb(base+2, ...)
    1134: outb(base+3, ...)   1135: outb(base+4, ...)   1136: outb(base+5, ...)
    1137: outb(base+7, 0x20)
⇒ 覆盖 `0x1F0,0x1F1,0x1F2,0x1F3,0x1F4,0x1F5,0x1F6,0x1F7` —— **全部落在主通道寄存器文件内** ✓
  （数据口 `inw(base)` 在 L1143，也是 0x1F0。）

## 3. 关于 `0xE0` 硬编码的裁决：**不是缺陷**

你的表述是『硬编码 0xE0，忽略 drv 形参』。**但 `ata_read_sector_dev` 没有 `drv` 形参**：

    1102: static bool ata_read_sector_dev(int dev, uint32_t lba, uint16_t* buf)
    1120: uint16_t base = dev ? 0x170 : 0x1F0;      // dev 只决定通道

而带 `drv` 的是**另一个函数族** `ata_*_disk(base, drv, ...)`，它确实写 `drv | ((lba>>24)&0x0F)`：

    1175: outb(base+6, drv | ((lba>>24)&0x0F));     // ata_read_sector_disk
    1188: outb(base+6, drv | ((lba>>24)&0x0F));     // ata_write_sector_disk

⇒ 在 `ata_read_sector_dev` 里 `dev=0` 意味着**主盘 master**，此时 drive/head 寄存器的应为
  `0xE0`（bit7=1, bit6=LBA, bit5=1, bit4=0=master）⇒ **`0xE0` 正是 dev=0 的正确值**。
  如果它真的用某个 `drv` 变量，反而可能把 slave(0xF0) 的位混进来。

**结论：这一条我判『当前无害且不是缺陷』** —— 它是「固定 dev=0」的等价写法，不是漏传参数。
  （若要支持 slave，正确做法是在 `dev` 语义上扩一位，而不是复用 0xE0 之外的常量。）

## 4. 关于「lba 高 4 位丢失」：**是真实缺陷，但当前无害**

这是**模拟器侧**的事实（`webemu/src/cpu_min.c`，**不在我的写作用域，仅作为证据引用**）：

    2442: static uint8_t g_ata_lba[4] = {0,0,0,0};
    2484: static uint32_t ata_lba(void){ return g_ata_lba[0] | [1]<<8 | [2]<<16 | [3]<<24; }
    2603-2605: case 3/4/5 -> g_ata_lba[0]/[1]/[2] = v;      // 只有低 3 字节被写
    2606: case 6 -> g_ata_drive = v;                        // 端口 6 存进另一个变量

⇒ **`g_ata_lba[3]` 全仓只被读（`ata_lba()`），从未被写** ⇒ `ata_lba()` 的高 8 位恒为 0。
   而内核写端口 6 的是 `0xE0 | ((lba>>24)&0x0F)`，低 4 位才是 LBA 高 4 位；
   **webemu 把整字节存进 `g_ata_drive`，从未把它的低 4 位并入 `g_ata_lba[3]`** ⇒ LBA 高 4 位丢失。

**为什么当前无害**：本镜像 SFS 最大 LBA ≈ **24,600**（SFS 在 LBA 6144 起、约 18,600 扇区），
远小于 `2^24 = 16,777,216` ⇒ 高 4 位恒为 0 ⇒ 丢失不可观测。

**判断：应修（但不紧急）**。理由：
1. 它是**静默截断**：一旦 LBA ≥ 2^24（128 GiB 镜像，或不远的磁盘扩容/多分区布局），读会**静默读错扇区**，
   而 SFS 会表现为『文件内容错乱』而不是报错 —— 与今天那条『把失败静默吞掉』同族的失败模式；
2. 修复代价极低（在 case 6 里多做一次 `g_ata_lba[3] = v & 0x0F`）；
3. **但它在 webemu 里**，属 gui-designer 的写作用域 ⇒ 我**只报告、不动手**（遵你给的作用域约束）。

## 5. 我在审计中发现的**一个额外事实**（task-37 的依据，顺带确认）

`FileEntry` **未 packed 时 `sizeof=36`，而磁盘项是 32**，但更精确的后果**不是等价的『越界读 4 字节』**：

    磁盘布局（sfs_gen.py / FS_ENTRY_SIZE=32）：
      name[20] @0   size @20   start_lba @24   type @28   parent @29(2B)   reserved @31
    未 packed 的结构（实测 offsetof）：
      name @0  size @20  start_lba @24  type @28  **parent @30**  reserved @32  sizeof=36

⇒ `size` 与 `start_lba` **恰好没有错位**（这也是 SFS 至今能用的原因）；
  但 **`parent` 偏移错 1 字节**（30 vs 29），且 `sizeof` 大 4（36 vs 32）。
⇒ 因此『越界读 4 字节』的表述**偏轻**：真正的危害是 **`parent` 被读成 `type` 的残留字节 + parent 低字节**，
  与 `reserved` 一起读错。完整修复（packed + static_assert）会同时校正这两者。

## 6. 未确定项 / 边界

- 本次反汇编基于 `build/kernel.elf`（710,xxx B）与 `kernel.bin` 同一次构建（21:32/14:40 两次构建均一致，sha16 `a98fbb90`）；
- 我没有跑 QEMU，因此『webemu 拿不到真数据』的**运行期**表现未由我复现（那是 gui-designer 的线）；
- `g_ata_lba[3]` 的结论为**全仓静态 grep**，未做运行期观测。
