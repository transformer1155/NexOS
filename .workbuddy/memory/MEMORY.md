# NexOS bootloader — 长期项目记忆（精简版）

## 构建环境 (强制)
- 所有 Linux 构建在 WSL: `wsl -e bash -lc 'cd /mnt/d/MyOS/bootloader && make <target>'`
- 工具链: nasm / g++ -m32 -ffreestanding / ld / objcopy / qemu-system-x86_64 / xorriso / mtools
- 镜像: `build/os.img`(BIOS raw, LBA33=kernel.bin) 无头测试; `build/os.iso` CD 布局(2026-08-05 起默认 make); UEFI 见下。看 C# GUI 用 `make play`(GTK)/`make bios-run`。
- Windows `rm` 被 safe-delete 拦 → 清旧产物在 WSL 内 `rm -f`。WSL/Windows 时钟偏移会让 make 跳过重编 → 改完先删再到 make。

## 架构 (双架构 32+64 位)
- 32 位 `kernel.bin`(LBA33, 链接 0x10000) 与 64 位 `kernel64.bin`(LBA2048, 链接 0x100000) 同存 `build/os.img`，SFS 在 LBA3328。
- 32→64: `switch64`(kernel.cpp:5019) 从 LBA2048 读 1024 扇区到 0x100000; 64→32: `switch32`(kernel64.cpp:2715) 从 LBA33 读 1024 扇区到 0x10000。**两方向 SECTORS 均=1024，勿改回 800/256**。
- VMM: 4MiB PSE 身份映射前 32MiB + VBE LFB; VGA 空洞 0xA0000–0xBFFFF; `USER_BASE=0x04000000..USER_END=0x08000000`; 统一 `-m 128M`。
- 已修复根因(勿踩): ① page_directory 与 .bss 重叠→三重故障, 现 `g_page_directory_store[1024](alignas 4096)`; ② .bss 尾落 VGA 空洞→已 relocate 到 1MiB+。

## GUI 纪律 (C# 优先, 2026-08-09 用户强制)
- 「要改的图形界面」尽量只改 C#(`csharp/`): 桌面/窗口/菜单/图标/托盘/右键菜单在 32 位 C# 托管壳(`NexOS.Forms` + `apps/Shell/*`)。
- 品牌统一 NexOS (PascalCase)。新增 App 用 `NexOS.Forms`/`NexOS.Core`，勿用 MiniOS/miniOS/nexos。
- C++ 内核层(gui.cpp/mforms.cpp/win32.cpp) 只留 host 回调注册、P/invoke 桥、ring-0 资源(fb/输入/窗口 surface)。
- C# 约束: 无浮点/泛型/接口/try-catch, 数组4B槽, 静态初始化器不执行(需 Shell.Init() 显式赋值), 堆每帧回卷; 动画状态存 static。

## 高频坑 (勿再踩)
- `handle_mouse_move` 中 `cursor.move` 只能调一次(否则位移×2)。
- fbcon 会清 LFB: gui_mode 下 `term.render()` 若调 `fb_console_render` 会抹 GUI → 已加 `if(g_wm.gui_mode) return;`。
- 退出 GUI 用 `gui_exit()` 保持 BGA+fbcon, 别 disable_vbe; 并 `fb_console_force_redraw()`。
- 16bpp 掩码: `((color>>19)&0x1F)<<11 | ((color>>10)&0x3F)<<5 | (color>>3)&0x1F`。
- 32 位内核须 `register_gui_callbacks()`(kernel.cpp cmd_gui), 否则 g_cb 全零。
- C# 控制键负向虚拟键: `0x08→-1`(Backspace)、`\n`/`\r`→`-2`(Enter); C# 的 OnKey 须同时接受负向与原始 ASCII。BrowserApp 曾只查 8/10/13 致 64 位退格回车失效(2026-08-10 已修)。
- CLR 字符串字面量上限: `clr.cpp` `CLR_MAX_STRINGS` 原 256→512; 加 App 报 `[CLR] fault: too many string literals` 时继续上调(同扩 `g_strobj[]`)。

## 测试工作流
- 安全套件 `make test-sec`(Foundation0/弹窗/Win32 GUI)。无头: `tools/test_ie_pe.py`/`test_ie_click.py`/`test_desktop_ui.py`/`test_kb_input.py`/`test_linux_compat.py`(4451); 跑完 `pkill -f "[q]emu-system"`。
- 验证: `term.write` 不写串口 → 屏幕靠 screendump, 内核逻辑靠 `serial_puts`。
- VirtualBox 真机式引导: BIOS 用 `MyOSTest`(32-bit,128MB,PIIX3,VBoxVGA,COM1→文件), `convertfromraw build/os.img build/os_vbox.vdi`; UEFI 用 `MyOSUefi`(64-bit,EFI64,128MB,PIIX4)。UEFI 启动链: `ST`→`[GOP]`→`[4]`→`[5]`→`EGI3SFB`→`[K1]`→`[K32] Entering Win11 GUI mode`。

## Win32 PE 加载器
- `run <file>`→`cmd_run`→winloader.cpp 仅 PE 检测(只报不跑)。`winapp <file>`→真正执行器: 32 位 `win32_run`(machine==0x014C && magic==0x010B, 否则 -3); 64 位 `win64_run`(MAXPE=192KiB 截断)。`win32_ensure_init()` 必在 PE 启动前调。
- 浏览器: 32 位 `iexplore.exe`(winpe/iexplore.c, 真实 PE32 i386); 64 位 = C# 托管 `BrowserApp`(默认 `https://www.bing.com/`)。验证地址栏编辑靠串口 `[browser] addr=`/`[iexplore] addr=` 长度序列, 不靠像素。

## 浏览器 IE: agent 桥 (2026-08-09)
- 私有消息 `WM_MINIOS_API=0x8000`: kernel `webapi` 命令(口令鉴权)向 IE 投递(verb=wParam, arg=lParam)。agent verbs: PING=1/GET_URL=2/NAVIGATE=3/GET_TEXT=4/ASK=5/CLICK=6/LINKS=7/STATUS=8。
- AI 桥: iexplore 惰性绑定 `MiniAiReady/MiniAiInit/MiniAiAsk`(→ kernel `kern_ai_*`→`ai_engine.cpp`, 无需外部模型, `ai_init` 恒成功)。口令 24 字节 XOR 混淆 `WEBAPI_PW`, `webapi auth <pw>` 解锁, ≥3 次失败锁死。

## 锁屏 + 默认 GUI + 取消 ESC (2026-08-10)
- 默认进图形: `kmain`/`kmain64` 在 `g_auto_gui && g_vbe_active` 时 `cmd_gui` 自动进桌面。`g_auto_gui` 默认 1, **勿被 TEMP-DIAG 改回 0**。
- ESC 不退终端: GUI 事件循环 0x1B 只交 `gui_handle_key`; `Login.Key` 遇 27 return。串口标记 `[GUI] ESC ignored`。
- 图形登录: `csharp/apps/Shell/Login.cs` 提交 `Host.LoginCheck`; 内核 `gui_cb_login_check` 比对哈希, `[K32-LOGIN] OK user=root`; 哈希不出内核。默认账户 root/admin、guest/guest。
- 文本终端已被 GUI「Terminal」窗口取代(桌面/开始菜单图标); 切 64 位: 登录后点 Terminal 图标输 `switch64`。

## 中文位图字体 (2026-08-11)
- 字库 `zfont_data.h`(GB2312 16×16, 387 字), `zfont_find_unicode()` 按 Unicode 码点二分。
- 根因: `gen_zfont.py` 原按区位码排序, 与运行时二分不一致 → 常用字命中错索引成方框。修复: 生成 `zfont_unicode[]` 按 Unicode 严格升序。扩充字库必须保证该表升序。

## UEFI 32 位兼容模式 + >4GB 帧缓冲 (2026-08-11 修复)
- `enter_kernel.S` 不退出 long mode, 切 IA-32e 32 位兼容模式沿用 UEFI 4 级页表。`>4GB` fb 必须在长模式内建 4 级页表。
- `bootuefi.c`: 已删无效 shadow(空壳导致黑屏), 保留真实 64 位 fb 地址交给内核映射。
- 内核 `vmm_init()` long-mode 分支: `framebuffer_phys64>4GB && shadow_buffer==0` 时自建 PML4(低 4GB 身份映射 + `0xF0000000` 虚拟窗口映射真实高 fb), 重写 `VbeInfo.framebuffer_phys=0xF0000000`、`shadow_buffer=1`。gui.cpp 凭 `shadow_buffer=1` 不 abort。
- VirtualBox Bochs VBE fb 恒 `<4GB`(通常 `0xE0000000`), 不触发该分支; 真机 Iris Xe 须 COM1 确认 `[VMM] high FB ... mapped to VA 0xF0000000`。
- **真机白线修复**: `present()` 全屏翻页原忽略 LFB `pitch`(Intel GOP `PixelsPerScanLine>HorizontalResolution` 时把图压进前几行); 已改四格式分支均按 `lfb + ry*pitch` 逐行写。`present_rect()` 本已正确。`[DGOP]` 串口诊断打印 fb_base/fb64/w/h/bpp/pitch/nominal 与 MATCH/MISMATCH。虚拟机 pitch==width*bpp 无法复现, 真机须看 `[DGOP]` 是否 `MISMATCH(pitch>width)`。
- **诊断构建**: `make uefi-diag [FB_TEST=1|2|3]` 产 `build/os_uefi_diag.img`(独立产物链 gui_diag.o/bootuefi_diag.o/kernel_diag.bin，不污染正常 `uefi`)。`[GOPF]` 打真实 GOP `PixelFormat` 枚举/`PixelsPerScanLine`/`PixelBitMask` 的 R/G/B 掩码；`gui_fb_diag()` 在 `gui_enter()` 入口画竖条/红块(FB_TEST=1)/全屏蓝(FB_TEST=2)/竖条用 width*4 pitch(FB_TEST=3) 后 `hlt` 停机。真机烧录抓 COM1 定性(竖条均匀=OK；红块不可见=>高fb映射失败；蓝屏仍白条=>pitch 偏差；`[GOPF]` 非标 BitMask=>扩格式分支)。QEMU/OVMF 跑不到 GUI(32位UEFI内核早期分页环境性不兼容, 复位在 GUI 前), 仅验证 bootloader+[GOPF] 启动。
