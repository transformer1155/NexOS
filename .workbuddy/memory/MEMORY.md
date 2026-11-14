# NexOS 项目记忆（精简）

## 构建/测试入口（必读）
- 唯一通道：`vm-run "<cmd>"`（`C:\windows\vm-run.bat` → SSH 到 VBox VM **Linux** Ubuntu；共享=`E:\NexOS\NexOS` ↔ VM 内 `/media/sf_NexOS/NexOS`）。宿主勿直接 make/gcc。
- 内核镜像：`make -j1 all MODEL_GGUF=build/qwen2-0_5b-instruct-q4_k_m.gguf`（不显式 MODEL_GGUF→缺权重，17MB 假图）。完整 `make all` 含 20 个 dotnet mex 应用，~20-25min。
- `vm-run push x.sh /tmp/x.sh` 须在 Git Bash 加 `MSYS_NO_PATHCONV=1` 且三参不带引号（否则 `/tmp/...` 被转成本地 temp 路径）；复杂逻辑写脚本后 `vm-run "bash /tmp/x.sh"`。
- 共享文件夹 mtime 滞后 → make 漏编：构建脚本里先 `touch` 改动源；`.o` 比源新会让 make 跳过重编（曾致 `nexos_compat64.o` 陈旧误报 check-k64）。
- **check-k64 门禁**：64 位内核 `--unresolved-symbols=ignore-all`，裸调用未定义符号→跳 0→三故障重启。`tools/check_k64_undefined.sh` 严格重链比对 `tools/k64_undefined_allowlist.txt`。坑：`g_compat_resolve_exact` 因陈旧 `.o` 被误报 bound-to-0，强制重编后 `0 (clean)`。
- VBox 测试 VM 名 **NexOS**（串口 uartmode1=file→`build/vbox_serial.log`）。同步：`tools/vbox_sync.ps1`（VM 须 poweroff，`VBoxManage controlvm NexOS poweroff` 硬断电，NexOS 不响应 acpi）。`python tools/vbox_check.py` 校验 IN SYNC。VBoxManage=`C:\Program Files\Oracle\VBoxManage.exe`。

## 内核态插件加载器 + `.nexbundle`（2026-11-14 落地）
- 四阶段交付：① `plugins/loader/plugin_loader.{h,cpp}`（能力位/沙盒/128 槽/句柄代际/`bnd_prefix_hit`）② `src/plugin_runtime.rs`（tokio 超时 abort、`CatchUnwind` 逐 poll 隔离、域名白名单、MCP JSON-RPC）③ `docs/MCP_PLUGIN_PROTOCOL.md` + `docs/NEXBUNDLE.md` + `docs/schemas/manifest.schema.json` ④ `plugins/samples/sysinfo/`（C# MiniCLR 插件 + UI + `build.sh`）。
- 验证入口：`make plugin-loader-selftest`（**33 passed**；`NEX_PL_HOST_TEST` 下同一份策略代码跑空宿主）；`cargo test --lib`（**13 passed**）；`python tools/image/nexbundle.py selftest`（22 项，含 RFC 8032 Ed25519 向量）；`bash plugins/samples/sysinfo/build.sh` 产出并 verify `dist/sysinfo.suite-1.0.0.nexbundle`。
- 句柄：`h=(gen<<8)|(slot+1)`；卸载/复用槽位都 `Gen++`，旧句柄 `PL_ERR_STALE(-4)`。未签名：先摘 `CAP_FS_WRITE`，根强制 `/sandbox/<id>`；路径读写都独立过 `bnd_prefix_hit`（`..`/反斜杠/兄弟前缀全拒）。
- 内核默认**不链入**加载器（128 槽注册表 = 约 8 KiB `.bss`，未做 `check_k64_fit`）：`make ... K64_PLUGIN_LOADER=1` 才追加 `plugin_loader64.o`。
- `.nexbundle` = ZIP 全 STORE + `manifest.json` + `plugins/<id>/{plugin.json,*.mex,ui/}`；`pack/verify` 同一套校验，确定性 ZIP（固定时间戳/排序），签名覆盖 `tree_sha256`，纯 Python Ed25519（无第三方依赖）。
- 坑：`plugin_loader.cpp` 在 freestanding 下 `<stdint.h>` 不保证 `NULL`，文件内自行 `#define NULL 0`；`pl_emit_int`/selftest 仅 `NEX_PL_HOST_TEST` 编译，避免把无用分支带进内核 `.text`。示例 csproj 必须带 `STANDALONE_APP`，否则 `Forms.cs` 会拉进 `Desktop/Voice` 分支；`Voice.cs` 不要直接加进插件工程。

## 机制层 `plugin_core`（2026-11-14 落地，五模块）
- 文件：`plugins/loader/plugin_core.{h,cpp}` + `core_selftest.cpp`（宿主 main + `osapi_*` 桩 + `pc_set_emit_hook(stdout)`）。与 `plugin_loader` 的 `pl_*` 完全独立，可串联：`pl_load_plugin` 校验 → `load_plugin` 建上下文 → `dispatch_mcp` 派发。
- 验证：`make plugin-core-selftest` → **68 passed, 0 failed**（场景 A 伪造指针 / B `/etc/passwd` / C 8 线程×8 插件真并发 / D 卸载后旧句柄 + 配额 + OOM + 200 路由回移）；`make build/plugin_core64.o` → **0 warning**，`.text=31520` `.bss=100488`。
- 机制要点：`spinlock_t` CAS+`pause`（内核态 `spin_lock_irqsave` 先 cli，恢复只在原 IF=1 时 sti）；句柄 `(gen<<8)|(slot+1)`、24 位代际、位图 `ctz` 分配；未签名插件影子页表硬重置为唯一 `/sandbox/<id>` RW 页（EXEC/额外页全丢）；FNV-1a + 512 槽开放寻址，**Knuth 回移删除不留墓碑**，每插件一条路由链，`unload` 一次摘链；并发配额 `inflight`（默认 10/硬上限 32）；128 槽满锁存 `g_oom_tripped`，需 `pc_clear_oom_fuse()`。
- **两个真坑**：① `is_valid_user_ptr()` 返回 `1` 合法 / 负值拒绝，调用点写成 `if (!rc)` 会把 `-5` 当成功 → 必须 `!= 1`（曾导致 `safe_copy_from_user(forged)` 段错误）；② 路由回移不能「遇到 `home==j` 就 break」，必须按环状区间 `can_move` 判断并**跳过不可搬条目继续扫描**，否则空洞会挡住后续同簇条目（200 路由 churn 的 `churn.t121` 曾因此命不中）。
- 默认不链入 kernel64（`.bss` 约 98 KiB）：`make ... K64_PLUGIN_CORE=1` 并先跑 `tools/verify/check_k64_fit.sh`。设计文档 `docs/PLUGIN_CORE.md`（含威胁模型边界：本层不做抢占/ring3，死循环靠运行时超时 abort，故障靠 `nexos_compat` IST+longjmp）。

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
