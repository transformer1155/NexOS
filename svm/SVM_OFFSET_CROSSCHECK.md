# SVM VMCB 偏移「三处同源」交叉审计（只读）

**任务**：Lead 派发（零槽、纯静态）　**作者**：kernel-architect　**日期**：2026-11-02
**动机**：svm-porter 的动态验证证明了「VMRUN 进 guest 到 HLT」，但**不能证明每个 offset 都正确**
（跑通可能是因为那条路径没用到某个错位字段）。本审计用静态对照补上这一层。
**对照基准**：本仓库参考结构 svm/svm_vmcb.h + AMD APM / QEMU svm.h 规范表（task-26 报告）。

## 0. 结论

- **控制区（svm-porter 本次修的 8 个常量）：三处完全一致，且全部符合规范** ✓
  含关键项 **VMCB_EXITCODE = 0x070**（旧值 0x058 已改正）—— 本次修复的核心，三处都到位。
- **保存区两个常量仍然错，且三处一致地错**（不是漏改某一处）：VMCB_SAVE_CR2、VMCB_SAVE_G_PAT。见 A1/A2。
- **本仓库参考结构 svm/svm_vmcb.h 反而是对的** ⇒ 分歧是「参考 vs 三处实现」，不是三处之间。
- VMCB_SAVE_G_PAT 在 kernel64.cpp 中根本不存在（A3），全树无使用点 ⇒ 目前无害。
- **两个字段是「三处一致但错、且被实际使用」**：VMCB_SAVE_CR2（A1）、VMCB_NEXT_RIP（A4）—— 只有静态审计能抓到。

## 1. offset 对照表（22 项，格式：规范 / svm_driver.c / vmm.cpp / kernel64.cpp / 判定）

| 常量 | 规范 | svm_driver.c | vmm.cpp | kernel64.cpp | 判定 |
|---|---|---|---|---|---|
| VMCB_IOPM_BASE_PA | 0x040 | 0x040 L75 | 0x040 L120 | 0x040 L6905 | 一致且合规 |
| VMCB_MSRPM_BASE_PA | 0x048 | 0x048 L76 | 0x048 L121 | 0x048 L6906 | 一致且合规 |
| VMCB_ASID | 0x058 | 0x058 L77 | 0x058 L122 | 0x058 L6907 | 一致且合规 |
| VMCB_TLB_CTL | 0x05C | 0x05C L78 | 0x05C L123 | 0x05C L6908 | 一致且合规 |
| VMCB_INT_CTL | 0x060 | 0x060 L79 | 0x060 L124 | 0x060 L6909 | 一致且合规 |
| VMCB_EXITCODE | 0x070 | 0x070 L80 | 0x070 L125 | 0x070 L6910 | 一致且合规（本次修复核心） |
| VMCB_EXITINFO1 | 0x078 | 0x078 L81 | 0x078 L126 | 0x078 L6911 | 一致且合规 |
| VMCB_EXITINFO2 | 0x080 | 0x080 L82 | 0x080 L127 | 0x080 L6912 | 一致且合规 |
| VMCB_NEXT_RIP | 0x0C8（见参考结构） | 0x0A0 L83 | 0x0A0 L128 | 0x0A0 L6913 | 三处一致，但**与参考结构冲突且被读取**（A4） |
| VMCB_EVENT_INJ | 0x0A8 | 0x0A8 L84 | 0x0A8 L129 | 0x0A8 L6914 | 一致且合规 |
| VMCB_SAVE_EFER | 0x4D0 | 0x4D0 L99 | 0x4D0 L147 | 0x4D0 L6927 | 一致且合规 |
| VMCB_SAVE_CR4 | 0x548 | 0x548 L100 | 0x548 L148 | 0x548 L6928 | 一致且合规 |
| VMCB_SAVE_CR3 | 0x550 | 0x550 L101 | 0x550 L149 | 0x550 L6929 | 一致且合规 |
| VMCB_SAVE_CR0 | 0x558 | 0x558 L102 | 0x558 L150 | 0x558 L6930 | 一致且合规 |
| VMCB_SAVE_DR7 | 0x560 | 0x560 L103 | 0x560 L151 | 0x560 L6931 | 一致且合规 |
| VMCB_SAVE_DR6 | 0x568 | 0x568 L104 | 0x568 L152 | 0x568 L6932 | 一致且合规 |
| VMCB_SAVE_RFLAGS | 0x570 | 0x570 L105 | 0x570 L153 | 0x570 L6933 | 一致且合规 |
| VMCB_SAVE_RIP | 0x578 | 0x578 L106 | 0x578 L154 | 0x578 L6934 | 一致且合规 |
| VMCB_SAVE_RSP | 0x5D8 | 0x5D8 L107 | 0x5D8 L155 | 0x5D8 L6935 | 一致且合规 |
| VMCB_SAVE_RAX | 0x5F8 | 0x5F8 L108 | 0x5F8 L156 | 0x5F8 L6936 | 一致且合规 |
| VMCB_SAVE_CR2 | 0x640 | 0x618 L109 | 0x618 L157 | 0x618 L6937 | 三处一致，但比规范低 0x28（A1） |
| VMCB_SAVE_G_PAT | 0x668 | 0x638 L110 | 0x638 L158 | 缺失 | 两处一致地错 + 一处没有（A2/A3） |

统计：**18 项一致且合规**；3 项见下（2 项是真错，1 项是规范外字段）。

## 2. 四条发现

### A1 VMCB_SAVE_CR2 三处一致地错（0x618，规范 0x640，低 0x28）—— 且它被实际使用
使用点（三处写法相同）：
    .attic64/kernel64.cpp:7123   vm_put32_(v, VMCB_SAVE_CR2, 0);
    drivers/svm_driver.c:427     vm_put32(v,  VMCB_SAVE_CR2, 0);
    plugins/vmm.cpp:558          vm_put32(v,  VMCB_SAVE_CR2, 0);
**后果**：本意是把保存区的 CR2 清零，实际写到 0x618 —— 按规范那是 **sfmask**（STAR/SFMASK 路径掩码）。
⇒ ① 预期的 CR2 初始化没有发生；② sfmask 被意外清零。
**为什么动态验证没抓到**：guest 路径未依赖该字段（到 HLT 不需要它）—— 正是「跑通 != 每个 offset 都对」的实例。
**参考结构是对的**：svm/svm_vmcb.h 的静态断言写明 cr2 @0x240（绝对 0x640）✓ 与规范一致。
建议照参考改三处：0x618 → 0x640。

### A2 VMCB_SAVE_G_PAT 两处一致地错（0x638，规范 0x668，低 0x30）
全树无使用点（只有定义）⇒ 目前无功能影响；但与 A1 同源（保存区偏移整体偏小）。
建议：一并改正，或在注释里显式标注「未使用，值待核」。

### A3 VMCB_SAVE_G_PAT 在 kernel64.cpp 中缺失
kernel64.cpp 里搜不到 G_PAT / 0x638 / 0x668 的任何出现。全树无使用点 ⇒ 目前无害；
但它意味着三处并非严格同源，将来若有人在 64 位路径用它，会立刻编不过或取到错值。

### A4 VMCB_NEXT_RIP = 0x0A0 —— 三处一致，但**与参考结构冲突，且被实际读取**（修正：这不是无害项）
使用点（三处都有，都是**读**）：
    .attic64/kernel64.cpp:7219  uint32_t next_rip = vm_get32_(v, VMCB_NEXT_RIP);
    drivers/svm_driver.c:553    uint32_t next_rip = vm_get32(v,  VMCB_NEXT_RIP);
    plugins/vmm.cpp:686         uint32_t next_rip = vm_get32(v,  VMCB_NEXT_RIP);
**本仓库参考结构给出的权威值不同**（svm/svm_vmcb.h）：
    L206  uint64_t ghcb_gpa;   // 0x0A0      <- 0x0A0 在参考结构里是 ghcb_gpa（SEV 字段）
    L213  uint64_t next_rip;   // 0x0C8
    L396  SVM_STATIC_ASSERT(offsetof(..., next_rip) == 0x0C8, "next_rip @0x0C8");
**结论**：三处把 next_rip 读作 **0x0A0**，而参考结构（带静态断言）说它在 **0x0C8**；
且 task-26 的规范表把 0x0A0 列为 reserved[16]（0x098-0x0A7）。
⇒ 这是一个**被使用中的错位字段**（不是「规范外但无害」）。
**建议**：三处 VMCB_NEXT_RIP 0x0A0 → **0x0C8**（与 svm_vmcb.h 的断言一致）。
**它为什么没在动态验证中暴露**：该字段大概只在特定 exit（如 IOIO/MMIO 模拟）路径上被用于推进 RIP；
而本次端到端走的是 HLT 路径（且先前的实验里 OUT 并未触发 trap）。

## 3. 与 svm-porter 动态验证的关系（为什么两者都需要）

| | 动态验证（他做的） | 静态审计（本报告） |
|---|---|---|
| 能证明 | VMRUN 真进 guest、到 HLT、exit_code=0x78 | 每个 offset 与规范/彼此的关系 |
| 不能证明 | 未被该路径使用的字段是否正确 | 运行期是否真的用到这些字段 |
| 本例价值 | 证明主路径通了 | **抓到 CR2 这类「跑通但错」的字段** |

## 4. 建议（按优先级）

| # | 建议 | 归属 | 风险 |
|---|---|---|---|
| 1 | 三处 VMCB_SAVE_CR2：0x618 → 0x640（与 svm_vmcb.h 的 cr2 @0x240 一致） | svm-porter（三处均其写作用域） | 低：只改常量；需一次构建 + 一次 boot 回归 |
| 2 | VMCB_SAVE_G_PAT：0x638 → 0x668，或显式标注「未使用、待核」 | 同上 | 低（当前无使用点） |
| 3 | 在 kernel64.cpp 补上 VMCB_SAVE_G_PAT，或注明「64 位路径不使用」 | 同上 | 无 |
| 3b | 三处 VMCB_NEXT_RIP：0x0A0 → **0x0C8**（与 svm_vmcb.h 断言一致） | svm-porter | 低：只改常量；但它**被读取**，改后需一次 boot 回归 |
| 4 | 把本表做成机器可跑的对照（从三处源码抽常量 vs 规范表） | 我（零槽） | 无：将来任何人改完一跑即知三处是否齐平 |

## 5. 边界（未确定项）

- 规范表取自 task-26 描述（QEMU target/i386/svm.h + AMD APM），本报告未重新联网核对上游原文。
- 只核对被 #define 的常量；若有「按结构体偏移直接写」的地方不在覆盖内。
- VMCB_CLEAN（0x0F8）在 task-26 标为 UNKNOWN，本报告未重复评估。

## 6. 机器可跑的对照（第 4 条建议的落地）

**脚本**：svm/check_vmcb_offsets.py　**运行**：python3 svm/check_vmcb_offsets.py（不符即 exit 1）

关键设计：**期望值只从 svm/svm_vmcb.h 的 SVM_STATIC_ASSERT 提取**（唯一在树内的权威），
保存区自动加 VMCB_SAVE_AREA_OFFSET（0x400）；**参考里没有断言的字段一律不猜**（标 NO REFERENCE）。

### 6.1 本次实际输出（2026-11-02）

```
reference field    VMCB macro     area     expected     svm_driver.c vmm.cpp      kernel64.cpp
iopm_base_pa       VMCB_IOPM_BASE_PA control  0x040        0x040        0x040        0x040        ok
msrpm_base_pa      VMCB_MSRPM_BASE_PA control 0x048        0x048        0x048        0x048        ok
asid               VMCB_ASID      control  0x058        0x058        0x058        0x058        ok
tlb_ctl            VMCB_TLB_CTL   control  0x05C        0x05C        0x05C        0x05C        ok
int_ctl            VMCB_INT_CTL   control  0x060        0x060        0x060        0x060        ok
exit_code          VMCB_EXITCODE  control  0x070        0x070        0x070        0x070        ok
exit_info_1        VMCB_EXITINFO1 control  0x078        0x078        0x078        0x078        ok
exit_info_2        VMCB_EXITINFO2 control  0x080        0x080        0x080        0x080        ok
event_inj          VMCB_EVENT_INJ control  0x0A8        0x0A8        0x0A8        0x0A8        ok
clean              VMCB_CLEAN     control  0x0C0        0x0F8        0x0F8        0x0F8        ALL THREE WRONG
next_rip           VMCB_NEXT_RIP  control  0x0C8        0x0A0        0x0A0        0x0A0        ALL THREE WRONG
efer               VMCB_SAVE_EFER save     0x4D0        0x4D0        0x4D0        0x4D0        ok
cr4                VMCB_SAVE_CR4  save     0x548        0x548        0x548        0x548        ok
cr3                VMCB_SAVE_CR3  save     0x550        0x550        0x550        0x550        ok
cr0                VMCB_SAVE_CR0  save     0x558        0x558        0x558        0x558        ok
rflags             VMCB_SAVE_RFLAGS save   0x570        0x570        0x570        0x570        ok
rip                VMCB_SAVE_RIP  save     0x578        0x578        0x578        0x578        ok
rsp                VMCB_SAVE_RSP  save     0x5D8        0x5D8        0x5D8        0x5D8        ok
rax                VMCB_SAVE_RAX  save     0x5F8        0x5F8        0x5F8        0x5F8        ok
cr2                VMCB_SAVE_CR2  save     0x640        0x618        0x618        0x618        ALL THREE WRONG
g_pat              VMCB_SAVE_G_PAT save    0x668        0x638        0x638        -            MISSING in 1 file
checked=21  mismatches=4  no-reference=0
```

### 6.2 ★ 新增发现：VMCB_CLEAN（第 5 项不符，也解决了 task-26 的 UNKNOWN）

- 参考结构断言 **clean @0x0C0**（svm_vmcb.h:395）；三处实现都写 **0x0F8**。
- 三处都在**实际写**它：vm_put32(v, VMCB_CLEAN, 0)（svm_driver.c:397/573/586、
  kernel64.cpp:7097/7236/7249 等），注释写「0 = all state dirty: always reload」。
- **当前无功能影响**：VMCB 在构建时被整体清零，所以 0x0C0 处的 clean 本来就是 0（语义恰好一致）；
  而写到 0x0F8 落在 reserved 空洞里，等于什么都没做。
- **但它是错的**：一旦将来有人依赖 clean 位的显式管理（例如只标脏部分状态），就会失效。
- ⇒ task-26 把 VMCB_CLEAN 标为 UNKNOWN（因为给的表把 0x0C0-0x3FF 整段列为 reserved）；
  **本仓库参考结构给出了答案：0x0C0**。建议把三处改为 0x0C0。

### 6.3 汇总：4 项不符

| 字段 | 参考（权威） | 三处实际 | 使用情况 | 当前影响 |
|---|---|---|---|---|
| clean | 0x0C0 | 0x0F8（三处） | 写 | 无（VMCB 已清零，语义巧合一致） |
| next_rip | 0x0C8 | 0x0A0（三处） | **读** | 读错位字段 |
| cr2 | 0x640 | 0x618（三处） | **写** | 清错字段（清了 sfmask），CR2 未初始化 |
| g_pat | 0x668 | 0x638（两处） | 无使用点 | 无（且 kernel64.cpp 缺定义） |

### 6.4 未覆盖的部分（脚本边界）

- 脚本只对照**参考里有断言的字段**。实现里存在但参考无断言的宏（如 VMCB_SAVE_DR7/DR6、
  VMCB_CR_INTERCEPT / VMCB_WORD3_INTERCEPT 等）**不在表内** —— 既不判 ok 也不判错（不猜）。
- 可选增强：列出「实现有定义、参考无断言」的宏清单，作为待核列表。
#### 6.2.1 ★ 「巧合安全」的成立条件（这是一条**有条件**的结论，不是保证）

`clean` 之所以暂时无害，依赖**两个前提**，二者都已在该代码路径上验证：

1. **VMCB 每次会话被整页清零**：
   `drivers/svm_driver.c:755-758` 的 VMCB/hsave/iopm/msrpm 全部来自 `pmm_alloc_page()` / `pmm_alloc_range()`
   （注释明写「4 KiB page aligned, so they come from the PMM, **not kmalloc**」），
   随后 `svm_build_vmcb()` 里 `svm_memzero(v, SVM_PAGE_SIZE)`（L376）**显式整页清零**；
   iopm/msrpm 同样显式清零（L787-788）。`kernel64.cpp` 与 `vmm.cpp` 亦同（L7077 / L506）。
2. **无人向真实的 0x0C0 写过非 0**：三处只会写 `VMCB_CLEAN = 0x0F8`（错位），从未触及 0x0C0。

⇒ 二者叠加，真实 `clean` 保持 0 ⇒「always reload」语义成立 ⇒ 当前无功能影响。

**但这是前提，不是保证**：
- 若将来 VMCB 的复用方式改变（例如多个 vCPU 复用同一页、或改成从不清零的分配器），该前提失效；
- 若将来有人把 clean 改为**显式脏标记管理**（只标脏部分状态），写错偏移会直接导致状态该重载而不重载；
- 若有人把该字段搬进一片**不被清零**的区域，则「残留脏位」会变成难查的行为差异。

⇒ 结论的准确措辞应是：**「在当前的清零+复用模型下巧合安全」，而不是「一直安全」**。
   这也是本次审计把 `clean` 仍列为**待修**（0x0F8 -> 0x0C0）的原因：
   它的安全性来自一个**外部条件**，而修好它之后安全性来自**它自己**。
