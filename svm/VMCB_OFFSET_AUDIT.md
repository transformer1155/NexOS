# VMCB 固定偏移审计（只读）

**任务**：task-26　**作者**：kernel-architect　**性质**：纯静态阅读，未编译、未跑 QEMU、未改任何源码。
**唯一写入**：本文件。

## 0. 结论一句话

**全树共发现 22 处偏移错误，分布在 3 个文件**：
drivers/svm_driver.c（10 处）、plugins/vmm.cpp（10 处，与前者同源）、
.attic64/kernel64.cpp（2 处；其控制区已由 svm-porter 本轮修正，仅 save 区 CR2/G_PAT 未修）。

错误模式**高度规整**，不是随机笔误：

- 自 iopm_base_pa 起 **LOW-by-0x10**：把 intercept 之后的 reserved[44] 当成 0x10 而非 0x30，
  且**漏掉了 tsc_offset (0x050)**
- 自 exit_code 起加深到 **LOW-by-0x18**：另外漏掉 int_vector (0x064) / int_state (0x068) 及其 reserved
- save 区另有 2 处独立错误：cr2 LOW-by-0x28、g_pat LOW-by-0x30

## 1. 逐个常量对照表

参照系：task-26 描述中的 QEMU/AMD 表（target/i386/svm.h，控制区恰为 0x400 B）。

### 1.1 drivers/svm_driver.c（#define，L59-L99）

| 行 | 我们的名字 | 我们的值 | 规范值 | delta | 判定 |
|---|---|---|---|---|---|
| 59 | VMCB_CR_INTERCEPT | 0x000 | 0x000 (cr_read u16) | 0 | CORRECT |
| 60 | VMCB_DR_INTERCEPT | 0x004 | 0x004 | 0 | CORRECT |
| 61 | VMCB_EXC_INTERCEPT | 0x008 | 0x008 | 0 | CORRECT |
| 62 | VMCB_WORD3_INTERCEPT | 0x00C | 0x00C（intercept u64 低半） | 0 | CORRECT |
| 63 | VMCB_WORD4_INTERCEPT | 0x010 | 0x010（intercept u64 高半） | 0 | CORRECT |
| 64 | VMCB_IOPM_BASE_PA | 0x030 | **0x040** | **-0x10** | **LOW-by-0x10** |
| 65 | VMCB_MSRPM_BASE_PA | 0x038 | **0x048** | **-0x10** | **LOW-by-0x10** |
| 66 | VMCB_ASID | 0x048 | **0x058** | **-0x10** | **LOW-by-0x10** |
| 67 | VMCB_TLB_CTL | 0x04C | **0x05C** | **-0x10** | **LOW-by-0x10** |
| 68 | VMCB_INT_CTL | 0x050 | **0x060** | **-0x10** | **LOW-by-0x10** |
| 69 | VMCB_EXITCODE | 0x058 | **0x070** | **-0x18** | **LOW-by-0x18** |
| 70 | VMCB_EXITINFO1 | 0x060 | **0x078** | **-0x18** | **LOW-by-0x18** |
| 71 | VMCB_EXITINFO2 | 0x068 | **0x080** | **-0x18** | **LOW-by-0x18** |
| 72 | VMCB_NEXT_RIP | 0x0A0 | 不在规范表内（0x0A0 属于 reserved[16] 0x098-0x0A7） | n/a | **NOT-IN-SPEC** |
| 73 | VMCB_EVENT_INJ | 0x0A8 | 0x0A8 | 0 | CORRECT |
| 74 | VMCB_CLEAN | 0x0F8 | 规范表内 0x0F8 属于 reserved[832]（0x0C0-0x3FF） | n/a | **UNKNOWN** |
| 88 | VMCB_SAVE_EFER | 0x4D0 | 0x4D0 | 0 | CORRECT |
| 89 | VMCB_SAVE_CR4 | 0x548 | 0x548 | 0 | CORRECT |
| 90 | VMCB_SAVE_CR3 | 0x550 | 0x550 | 0 | CORRECT |
| 91 | VMCB_SAVE_CR0 | 0x558 | 0x558 | 0 | CORRECT |
| 92 | VMCB_SAVE_DR7 | 0x560 | 0x560 | 0 | CORRECT |
| 93 | VMCB_SAVE_DR6 | 0x568 | 0x568 | 0 | CORRECT |
| 94 | VMCB_SAVE_RFLAGS | 0x570 | 0x570 | 0 | CORRECT |
| 95 | VMCB_SAVE_RIP | 0x578 | 0x578 | 0 | CORRECT |
| 96 | VMCB_SAVE_RSP | 0x5D8 | 0x5D8 | 0 | CORRECT |
| 97 | VMCB_SAVE_RAX | 0x5F8 | 0x5F8 | 0 | CORRECT |
| 98 | VMCB_SAVE_CR2 | 0x618 | **0x640** | **-0x28** | **LOW-by-0x28** |
| 99 | VMCB_SAVE_G_PAT | 0x638 | **0x668** | **-0x30** | **LOW-by-0x30** |

### 1.2 plugins/vmm.cpp（#define，L97-L140）—— 与 1.1 逐值相同（同源）

**完全相同**的 10 处错误 + 相同的 NEXT_RIP / CLEAN 标注，不再重复列表；差别仅整体行号 +38。

### 1.3 .attic64/kernel64.cpp（#define ...u，L6848-L6898+）

| 行 | 名字 | 我们的值 | 规范值 | delta | 判定 |
|---|---|---|---|---|---|
| 6866 | VMCB_IOPM_BASE_PA | 0x040 | 0x040 | 0 | CORRECT（已修） |
| 6867 | VMCB_MSRPM_BASE_PA | 0x048 | 0x048 | 0 | CORRECT（已修） |
| 6868 | VMCB_ASID | 0x058 | 0x058 | 0 | CORRECT（已修） |
| 6869 | VMCB_TLB_CTL | 0x05C | 0x05C | 0 | CORRECT（已修） |
| 6870 | VMCB_INT_CTL | 0x060 | 0x060 | 0 | CORRECT（已修） |
| 6871 | VMCB_EXITCODE | 0x070 | 0x070 | 0 | CORRECT（已修） |
| 6872 | VMCB_EXITINFO1 | 0x078 | 0x078 | 0 | CORRECT（已修） |
| 6873 | VMCB_EXITINFO2 | 0x080 | 0x080 | 0 | CORRECT（已修） |
| 6874 | VMCB_NEXT_RIP | 0x0A0 | 不在规范表内 | n/a | **NOT-IN-SPEC** |
| 6875 | VMCB_EVENT_INJ | 0x0A8 | 0x0A8 | 0 | CORRECT |
| 6898 | VMCB_SAVE_CR2 | 0x618 | **0x640** | **-0x28** | **LOW-by-0x28**（源码注释已标记、未修） |
| ~6899 | VMCB_SAVE_G_PAT | 0x638 | **0x668** | **-0x30** | **LOW-by-0x30**（同上） |

> 控制区 8 处已由 svm-porter 于本轮修正；save 区 CR2/G_PAT 仅注释标记、未修（其报告已声明）。

### 1.4 svm/svm_vmcb.h（struct + 注释）—— CORRECT

以结构体布局表达，注释值与规范一致：iopm_base_pa // 0x040、msrpm // 0x048、tsc_offset // 0x050、
asid // 0x058、exit_code // 0x070、exit_info_1 // 0x078、exit_info_2 // 0x080、nested_cr3 // 0x0B0。
**无 delta**。其中 next_rip // 0x0C8 不在本次规范表内（属于 reserved[832]）⇒ **UNKNOWN（若从未读写则无害）**。

### 1.5 其它文件（未发现固定偏移常量）

| 文件 | 情况 |
|---|---|
| svm/test_svm_static.cpp | **编译期 offsetof 断言**：iopm 0x040 / msrpm 0x048 / nested_cr3 0x0B0 / nested_ctl 0x090 / save.efer 0x0D0 ⇒ 编码的是**正确**值（全树唯一自检器） |
| tools/verify/svm_host_test.asm | save 区段写入 0x400/0x410/0x420/0x430/0x440/0x450 ⇒ 与规范一致（抽样，+19 行未逐行核对） |
| .attic64/entry64.asm、switch32to64.asm | **不含** VMCB 固定偏移（0 处匹配） |
| svm/svm_offsets.cpp、svm_vmcb_init.c/.h、svg/test_vmcb_init.cpp、tools/read_svm_log.py、svm/check_svm_static.sh、svm_bitmaps.h、svm_npt.h | 未发现硬编码固定偏移常量 |

## 2. 哪些文件进 32 位、哪些进 64 位

| 文件 | 32 位路径 | 64 位路径 | 依据 |
|---|---|---|---|
| plugins/vmm.cpp | **是**：$(BUILD)/vmm.o 在 PLUGIN_OBJS（Makefile L250） | **是**：$(BUILD)/vmm64.o 在 KERNEL64_OBJS（L1777 / L1799） | 同一源文件编两次 ⇒ **其 10 处错误同时影响两个位宽** |
| drivers/svm_driver.c | 不直接链接 | 不直接链接 | Makefile L255 明写 "Driver images (svm_driver.c) are NOT linked here -- they are separate"；它是**运行时加载的驱动映像**（SFS 中的 svm.drv，8,744 B），影响加载它的那一位宽 |
| .attic64/kernel64.cpp | 否 | **是**（独占） | 仅进 kernel64.elf |
| svm/svm_vmcb.h 及其 test/offsets 文件 | 否 | 否 | **均未出现在 Makefile 构建规则里**（svm_vmcb_init.c / svm_offsets.cpp / test_*.cpp 零命中）⇒ 静态/测试用，不进任何内核 |
| tools/verify/svm_host_test.asm | 否 | 否 | 宿主侧测试工具 |

## 3. 明确未能确定的项（不掩饰）

1. **VMCB_CLEAN 0x0F8**（svm_driver.c:74，vmm.cpp 同源）：本任务给定的规范表把 0x0C0-0x3FF 整段列为 reserved[832]，
   未单列 clean 位。我**无法仅凭给定表判定**其对错；需查 AMD APM Vol2 B.2 或 QEMU svm.h 全文。
   ⇒ 标 **UNKNOWN**，不建议在确认前改动。
2. **VMCB_NEXT_RIP 0x0A0**（三处）：给定规范表内无此字段，它落在 reserved 区。若代码从不读写则无害；
   我未逐处检查读写点（超出「常量对照」范围）⇒ 标 **NOT-IN-SPEC**，同样不建议先改。
3. tools/verify/svm_host_test.asm 只抽样核对了 16 处（save 区段选择子），**+19 行未逐行核对**。
4. svm/svm_vmcb.h 的 next_rip // 0x0C8 同上（不在规范表内）。

## 4. 给后续修复者的两条建议（不属于本任务范围）

1. **修 32 位路径前，先让 64 位路径的实验通过** —— 这正是 Lead 定下的顺序，我同意：
   同一份常量在三处出现，只有 64 位实验证明了某偏移正确，才该据此改 32 位。
2. **svm/test_svm_static.cpp 的全树唯一自检器值得扩展**：它已能断言 iopm / msrpm / nested_cr3 / nested_ctl / save.efer，
   建议把 asid / int_ctl / exit_code / exit_info_1 / exit_info_2 / cr2 / g_pat 也加进去 ——
   这 7 个字段正是本次审计出错的地方，加了断言即可在编译期拦住同类回归。
