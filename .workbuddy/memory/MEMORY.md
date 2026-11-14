# NexOS 项目记忆（精简 · 2026-11-14 整理）

## 构建 / 验证入口（必读）
- 唯一构建通道：宿主 `vm-run "<cmd>"`（`C:\windows\vm-run.bat` → SSH 到 VBox VM **Linux** Ubuntu；共享 `E:\NexOS\NexOS` ↔ VM `/media/sf_NexOS/NexOS`）。宿主勿直接 make/gcc。
- 异步编译：`tools/async_build.ps1 start|status|log|wait|stop [-Jobs 1]`（前端）→ `tools/async_build.sh`（VM 内 `setsid` + `flock /tmp/nexos_vm.lock` + `make -j2 build/os_v2.img`），状态/日志/心跳在 `build/async_build.*`，保护标记 `build/.building`（僵死只报警不自动删）。
- 完整镜像：`make -j1 all MODEL_GGUF=build/qwen2-0_5b-instruct-q4_k_m.gguf`；`MODEL_GGUF` 指向不存在路径 → 不嵌模型，os_v2.img 约 31MB。
- `vm-run push x.sh /tmp/x.sh` 需 Git Bash 加 `MSYS_NO_PATHCONV=1` 且三参不带引号；复杂逻辑写脚本再 `vm-run "bash /tmp/x.sh"`。
- 共享目录 mtime 滞后 → make 漏编：改动源后先 `touch`；`.o` 比源新会跳过重编（曾致 check-k64 误报）。
- **check-k64 门禁**：64 位内核 `--unresolved-symbols=ignore-all`，裸调用未定义符号→跳 0→三故障重启。`tools/check_k64_undefined.sh` 比对 `tools/k64_undefined_allowlist.txt`，须 `0 (clean)`；另有 `tools/verify/check_k64_fit.sh` / `check_kernel_sizes.sh`。
- VBox 测试 VM 名 **NexOS**（串口 uartmode1=file→`build/vbox_serial.log`）。换镜像：`VBoxManage controlvm NexOS poweroff`（不响应 ACPI，须硬断电）→ `powershell tools/vbox_sync.ps1`（TEMP 副本 convertfromraw 避免 VERR_SHARING_VIOLATION；`.building` 存在时拒绝同步；try/finally 重新 attach）→ `python tools/vbox_check.py` 校验 IN SYNC。VBoxManage=`C:\Program Files\Oracle\VBoxManage.exe`。
- VM 卡死（ssh banner 超时 / VERR_SHARING_VIOLATION on obj/Release/*.dll）：`controlvm Linux poweroff` → `startvm Linux --type headless`。

## 当前 UI 架构（2026-11-14）
- **Demo-only 模式**：`NEX_DEMO_ONLY ?= 1`（Makefile 默认）。`SHELL_MEX` 置空、`STANDALONE_MEX` 只留 `sfs_files/Demo.mex`、`SFS_WILD` 过滤掉所有 `.mex`（避免 wildcard 触发 20 个 csproj）。`-DNEX_DEMO_ONLY` 只注入 `mforms64.o`。
- `mforms_start()` 不再 `clr_load("shell.mex")`，改加载 `Demo.mex` 并调 `App.Main::Init`；`demo_fullscreen_paint()` 全屏绘制，wall/desktop 缓存禁用、每帧重绘；`mforms_desktop_click/key` 转发 `App.Main::Click/Key`；`mforms_has_desktop()` 仍返回 1。
- UI 源码：`csharp/apps/Shell/Demo.cs`（由 `csharp/apps/standalone/Demo` 工程产出 `sfs_files/Demo.mex`）+ `csharp/NexOS.Forms/Forms.cs`（`T` 主题 tokens Light/Dark/Ocean/Rose、`W` 控件、`Btn.SetPressScale` 默认 100 不缩小、`Gfx.Glass` 液态玻璃、`Host.TickMs()` 整数三角波边缘动画）。
- native 绘制：`src/gui/gfx_draw.inc` / `gfx_graphics.inc`（AA SDF coverage、半像素偏移、`blend_pixel` alpha clamp）。`Graphics` 在 `gui_legacy.cpp` 匿名命名空间 → `gui.cpp` 用同 TU include 拼装 `gui_selftest.cpp`；`gui_enter()` 开头跑一次 `gui_run_selftest()`，串口 `[GUI-TEST] ... PASS/FAIL`。`gui_sio_flush()` 64 位分支补 UART 0x3FD THRE 等待防 FIFO 丢字符。

## 架构要点
- 64 位内核 `kernel64.cpp`（kernel.elf）；MiniCLR 托管层 `.mex`；`mforms_*` native↔C# icall，注册名须与 C# 方法逐字一致；`CLR_MAX_ICALLS=384`、`CLR_MAX_VMS=12`、`NEX_APP_HANDLE_BASE=64`。`clr_new_str` 在 mforms_init 阶段返回 0 → 走 C 字符串 API，icall 自测易假绿。
- 像素格式 `0x00RRGGBB` 无 alpha；帧缓冲 VBE 描述符 0x5000；SFS≈9.96MB；`sfs_files/apps.man`（kind|名称|mex|色|字母|说明）为应用清单真源，内核 `apms.inc` 解析（`APMS_APP_MAX=32`），`app_registry.h` 为编译期镜像。
- 64 位内核 identity-map physical==virtual（`big_alloc`），`pmm_alloc_range()` 的 uint32 可直接当指针。
- 三重故障回归已修：`fault_ist_setup()`/`gdt_install_tss()` —— 显式 `lgdt gdt64`、TSS access byte 写描述符 +5、IST1 偏移 36 且指向栈顶。

## 既有子系统（已完成，细节见 docs/）
- 内核态插件：`plugins/loader/plugin_loader.*`（能力位/沙盒/128 槽/句柄代际）、`plugin_core.*`（spinlock/路由回移/inflight 配额）、`plugin_bridge.*`（weak 符号接线 + icall `NexOS.Plugin::Dispatch/Count`）、`malicious_plugin.*` + `breach_test.cpp`。`K64_PLUGIN_CORE ?= 1` 默认链入（+.bss 93552，余量约 130544B）。验证：`make plugin-loader-selftest`(33)、`plugin-core-selftest`(68)、`plugin-core-breach`(40)。边界见 `docs/PLUGIN_CORE.md` §5（无 ring3/抢占/分页隔离）。
- `.nexbundle`：ZIP 全 STORE + manifest + 确定性时间戳 + 纯 Python Ed25519，`tools/image/nexbundle.py`；`plugins/samples/sysinfo/` 示例。
- OS API L0–L3 + L12 配置存储（错误通道 per-token、全局句柄表、VFS/文件/进程/Glob/Cfg）已实机 `fails=0`；headless 需在 `headless_run()` 显式跑全部 `osapi_ic_*_self_test`。
- AI Win32 兼容层 D0–D4+E **骨架已编译但执行路径未接线**：`compat_pe_start/note_call/pe_yield` 零调用点，`win64_run` 仍同步。
- HEADLESS_STAGE 0/1/2/3 开关只注入 `build/kernel64.o` 编译行，勿进 `CXX64FLAGS`。

## 常见坑速查
- freestanding 下 `<stdint.h>` 不保证 `NULL`；未用静态函数要 `__attribute__((used))`（`maybe_unused` 对函数无效会告警）。
- `plugin_bridge.cpp` 必须 `#include "clr.h"`，不要手写 `extern "C" clr_str/clr_new_str` 原型（已修）。
- `is_valid_user_ptr()` 返回 `1` 合法 / 负值拒绝，调用点须 `!= 1`。
- `gui_cb_list_files()` 返回条目数不是字节数。
- 示例 csproj 必须带 `STANDALONE_APP`，否则 `Forms.cs` 拉进 Desktop/Voice 分支。
