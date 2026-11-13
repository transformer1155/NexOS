# NexOS 项目记忆（精简）

## 构建/测试入口（必读）
- 唯一通道：`vm-run "<cmd>"`（`C:\windows\vm-run.bat` → SSH 到 VBox VM **Linux** Ubuntu；共享=`E:\NexOS\NexOS` ↔ VM 内 `/media/sf_NexOS/NexOS`）。宿主勿直接 make/gcc。
- 内核镜像：`make -j1 all MODEL_GGUF=build/qwen2-0_5b-instruct-q4_k_m.gguf`（不显式 MODEL_GGUF→缺权重，17MB 假图）。完整 `make all` 含 20 个 dotnet mex 应用，~20-25min。
- `vm-run push x.sh /tmp/x.sh` 须在 Git Bash 加 `MSYS_NO_PATHCONV=1` 且三参不带引号（否则 `/tmp/...` 被转成本地 temp 路径）；复杂逻辑写脚本后 `vm-run "bash /tmp/x.sh"`。
- 共享文件夹 mtime 滞后 → make 漏编：构建脚本里先 `touch` 改动源；`.o` 比源新会让 make 跳过重编（曾致 `nexos_compat64.o` 陈旧误报 check-k64）。
- **check-k64 门禁**：64 位内核 `--unresolved-symbols=ignore-all`，裸调用未定义符号→跳 0→三故障重启。`tools/check_k64_undefined.sh` 严格重链比对 `tools/k64_undefined_allowlist.txt`。坑：`g_compat_resolve_exact` 因陈旧 `.o` 被误报 bound-to-0，强制重编后 `0 (clean)`。
- VBox 测试 VM 名 **NexOS**（串口 uartmode1=file→`build/vbox_serial.log`）。同步：`tools/vbox_sync.ps1`（VM 须 poweroff，`VBoxManage controlvm NexOS poweroff` 硬断电，NexOS 不响应 acpi）。`python tools/vbox_check.py` 校验 IN SYNC。VBoxManage=`C:\Program Files\Oracle\VBoxManage.exe`。

## AI 辅助 Win32 兼容层（D0–D4 + E，骨架已编译进镜像，**执行路径尚未接线**）
- ⚠️ **状态（2026-11-13 实测 ELF 符号表）**：`compat_pump`/IAT 登记/fault 钩子已接线并链接；但 `compat_pe_start`/`compat_note_call`/`compat_pe_yield` **只有定义、零调用点**。`win64_run` 仍是同步 `int rc = fn();`，PE 运行期间帧循环照旧冻结，AI 观测不到；`compat_pe_start` 复用 slot 还漏重置 `p->started`（第二次启动会 longjmp 进旧 PE 上下文）。用户拍板的「加载同步、执行异步」**未落地**。当前交付镜像 = 骨架版，`make all` RC=0 / `check-k64 0 (clean)` / VDI IN SYNC，用户自测中；后续接线建议用 `winapp /a` 显式异步开关隔离风险，勿改默认同步路径。
- 文件：`nexos_compat.h`(契约) / `nexos_compat.cpp`(实现) / `nexos_compat_asm.cpp`(64位 setjmp/longjmp/fiber_switch，`__attribute__((naked))`+内联 asm，`CompatCtx` 布局 rbx,rbp,r12,r13,r14,r15,rsp,rip)。
- D0 协作式 PE 执行：`compat_pe_resume/yield/abort_pe/entry_trampoline`，`g_kernel_ctx`/`g_pe_ctx` 两 `CompatCtx` 切换；`g_in_pe_exec` 守卫防误 longjmp；yield/abort 用 val=1。
- D1 观测环：`g_iat[]`/`g_calls[]`/`g_ring[]`，每帧 `mforms_paint_overlay()`→`compat_pump(4)`；MKFS 数据盘双缓冲 `ai_events.bin`/`ai_cmd.bin`。
- D2 热补丁：`compat_patch_iat`（内核命令+icall），AI 只选内置实现/哑句柄，**禁止下发机器码**。
- D3 降级哑值：共享 `compat_dummy_return`（L2/L3 经 `g_iat[]` 判定返回 0）+ `g_dummy_pool`。
- D4 故障可恢复：`fault_common()` 改 `cli;hlt` → `compat_on_fault`+`compat_abort_pe` 经 longjmp 安全返回；IST=1 故障栈（`gdt_install_tss()` 运行时填 TSS 描述符，`build_idt()` fault 向量 `ist_flags=0x8E01`）。
- E 类运维（E1–E5）：复用 `compat_pump`/`g_iat`/`g_calls`/`ai_events.bin` 做健康采集/诊断表/泄漏哨兵/灰度回滚/质量摘要。
- 内存映射：64 位内核 **identity-map physical==virtual**（见 `big_alloc`），故 `pmm_alloc_range()` 返回的 uint32 物理地址可直接当指针。当前源码为 `p->stack=(uint64_t*)pmm_alloc_range(...)`（编译有 `-Wint-to-pointer-cast` 警告，运行时正确；`(uintptr_t)` 强转版曾试改后已回退，保持与镜像一致）。

## OS API 升级线（L0/L1/L2/L3/L12 已实机验证）
- L0–L3 + **L12 配置存储**已落地：错误通道 per-token、全局句柄表、VFS/文件层、进程/Glob、Cfg 持久化。`osapi.cpp` 含 `osapi_proc_spawn_c`/proc_*/`osapi_ic_fs_glob`/`osapi_cfg_*_c`。串口自测全 `fails=0`，`[SHELL] osapi L12 cfg ok=1`。
- L12 要点：`osapi.h` 的 `ICALL_GUARD(cap,cls)` 宏（L12 首个完整使用，**L2/L3 存量 icall 补挂仍待办**）；Cfg 逻辑路径 `/data/cfg/<name>.cfg` 做边界检查，但 MKFS flat 表 `FS_NAME_LEN=20` → 物理名用 `<name>.cfg`；Flush 走 `.tmp`→rename；Close 自动 Flush + gen++ → Stale。设计文档写 `long` 句柄，实际因 MiniCLR 32 位栈机统一用打包 `h_t` 的 `int`（已记入文档 ABI 偏差）。`caps_mask` 只广告真实落地层。
- 关键坑：icall 注册名须与 C# 方法逐字一致；`clr_new_str` 在 mforms_init 返回 0→走 C 字符串 API；`CLR_MAX_ICALLS` 192→384。
- 实机补记（2026-11-13，L0–L3 串口自测全 `fails=0`）：`gui_cb_list_files()` 返回**条目数**不是字节数，Glob 曾把返回值当长度用而只看第一条，已改扫 NUL；自测走 icall 会因 `clr_new_str`=0 假绿，须直接调 C 侧 `osapi_fs_glob_c`。VBox 启动需宿主提交内存充足（NexOS VM 已从 4096MB 降到 1024MB）。
- 64 位内核三重故障回归已修（`fault_ist_setup()` / `gdt_install_tss()`）：① GDTR 仍停在 32 位 3 项 GDT（`0x11048:0x17`）导致 `ltr 0x28` #GP，改为从 `gdt64`/`gdt64_end` 显式 `lgdt`；② TSS access byte 必须写描述符 **+5**（原写 +7 是 base[31:24]）；③ IST1 架构偏移是 **36**（entry64.asm 布局多 4 字节），且要指向 `g_fault_stack+1024` 栈顶。

## V2.1 Headless 阶段（2026-11-13 实机全绿）
- 编译开关 `HEADLESS_STAGE`（Makefile，默认 0 = 旧 GUI 生路；1 = hlt；2 = +PIT/8259A 时钟 IRQ；3 = +双内核线程）。**只注入 `build/kernel64.o` 的编译行**，进 `CXX64FLAGS` 会静默混编不同 stage 的 .o。
- `entry64.asm::irq0_stub` 必须独立于 256 个通用 stub（那些全汇入 `fault_common`→`cli;hlt`）。同特权级中断只压 rip/cs/rflags，直接 call C 会覆盖 iret 帧 → 用专用 IRQ 栈；中断上下文用**无锁** raw 串口（`serial_puts` 的 TAS 锁在 IF=0 下会死锁）。PIC 必须重映射 32/40 且只放行 IRQ0。
- headless 跳过 `mforms_init()` → 必须在 `headless_run()` 里显式跑全部 `osapi_ic_*_self_test`，否则 L0–L12 的 `fails=0` 全部消失。所有 headless 符号**无条件编译**，否则 stage 0 时 `check-k64` 报未定义。
- 不嵌模型构建：`MODEL_GGUF` 指向不存在路径 → `os_v2.img` 429MB→31MB，内核可独立启动；`vbox_sync.ps1` 后旧的 412MB VDI 存在 `.prev`。
- **未做**：VMM/PML4/按需分页、ring3、ATA 异步化、模型正式外置、APIC/LAPIC Timer。详见 `docs/NexOS内核演进-V2.1-Headless落地记录.md`。

## 架构要点
- 64 位内核 `.attic64/kernel64.cpp`（产物 kernel.elf）；桌面 shell.mex（`Shell::Init`←`mforms_start`）。MiniCLR 不跑静态字段初始化器→哨兵显式 Init。
- 应用清单唯一真源 `sfs_files/apps.man`（kind | 名称 | mex | 色 | 字母 | 说明），内核 `apms.inc` 启动解析（`APMS_APP_MAX=32`）；`app_registry.h` 是编译期镜像（AppType↔Kind↔mex）。加应用要同时改：`apps.man`、`gen_standalone_apps.py`(APPS/KIND_OF)、`Shell.cs Kind.*`、`Shared/Shim.cs`、`Desktop.cs`(KINDS/kName/LoadDefaults/KindStyle/KindForTarget)、`app_registry.h`、`gui_legacy.cpp`(APP_* + 两个 switch + 窗口尺寸)。
- AI 工作台（kind 20 / APP_AIWORKBENCH 25 / `AiWorkbench.mex`，760×540）：4 个 MKFS 会话 `aiws1..4.txt`、`ws_*` 工作区文件、只读 shell 白名单、`/` 命令 + `[SHELL]/[READ]/[WRITE]/[DOC]/[LS]/[SEARCH]/[WEB]` 指令（3 轮）。见 `docs/AI_WORKBENCH_PORT.md`。
- 帧缓冲 VBE 描述符 0x5000；直写 LFB 须按真实 bpp（`fb_fill_rect()`）。
- 模型 blob LBA61440/61441；`ata_read_multi(count>1)` 禁用（VBox PIIX3 忙等）；BM-DMA PRDT 三处错误已修。
- SFS≈9.96MB；`sfs_files/apps.man` 应用清单真源；CLR slot `CLR_MAX_VMS=12`，`NEX_APP_HANDLE_BASE=64`。
