# NexOS Linux 兼容层 — Stage 6 交接文档（内核内 ELF 动态链接器 + libc.so）

> 交接对象：code buddy / 续作工程师
> 当前状态：**Stage 6 已实现并通过无头验证（9/9 CHECKS PASS）**，待用户最终 OK 收工。
> 编写日期：2026-08-24
> 工作目录：`D:\MyOS\bootloader`（纯 Windows 构建，无 WSL）

---

## 0. 一句话背景

NexOS 的远期目标是跑真·《我的世界》Java 版。为此需要一条渐进式路线：

```
动态链接 + 最小 libc.so  →  musl 适配  →  JVM/Hotspot 引导桩
   →  Mesa/3D GL  →  LWJGL  →  真·MC Java 版
```

**Stage 6 = 路线第一片**：让 freestanding ELF32 guest 通过动态链接调用共享库 `libc.so` 导出的函数（`printf` / `nex_add`），证明「内核内 ELF 动态链接器」闭环可用。后续 Stage 在此基础上换 musl、接 JVM。

---

## 1. 现有架构（必读，避免重复踩坑）

### 1.1 运行模型
- 32 位内核，guest（Linux 兼容层）跑在 **RING 0**，freestanding ELF32。
- 系统调用入口：`int 0x80` → `sys_enter`（syscall.cpp）→ C 侧 `linux_syscall_dispatch(SysRegs* r)`（linux_compat.cpp）。
- guest 文件从 Linux 分区（`linux_fs`，挂载于 **LBA 12288 / `SFS_LINUX_LBA`**）读取，由 `g_reader(name, buf, sz)` 提供；未挂载则回退主 SFS。
- guest 地址空间（64–256MiB 为 `PG_USER` identity-map，32 位非 PAE，**无 NX → 用户页天然可执行**）：
  - ELF image：`0x08048000 .. max_end`（~0x0814C000）
  - brk heap：`0x0814C000 .. 0x09000000`
  - argv strings：`0x09000000 .. 0x0A000000`
  - **`.so` 加载区（Stage 6 新增）**：`DYN_LIB_BASE = 0x0A400000`（字符串区之上、栈之下）
  - guest stack top：`0x0C000000`
  - mmap arena：`0x0C100000 .. 0x0FFFF000`

### 1.2 动态链接器设计要点（本 Stage 核心）
- **没有 `PT_INTERP` / ld-linux**：内核自身当链接器。
- 流程：`linux_dynload_and_exec` 读主程序 ELF → 算 `main_bias` → 映射主程序 → `dyn_parse` 找 `DT_NEEDED` → 逐个映射 `.so` 并 `dyn_apply(lib, mods, nmods+1)` 让 lib 自解析 → 主程序 `dyn_apply(&mods[0], mods, nmods)` 解跨对象重定位 → 设 `AT_BASE` → 跳主程序 entry。
- 主程序编 **PIE**（`-fPIE -pie`），内核算 `main_bias = 0x08048000 - (first_v - first_o)`，使其落入 0x08048000 区；`mods[0].base = main_bias`，RELATIVE 重定位 `*slot += base`。
- 支持的重定位类型：`R_386_NONE/R_32/R_PC32/R_GLOB_DAT/R_JMP_SLOT/R_RELATIVE`。
- 符号解析：`DT_HASH` 第二项 `nchain` == `.dynsym` 条目数；`dyn_resolve` 遍历所有已加载 mod 的导出符号（跳过 `st_shndx==0` 的 undefined）。

---

## 2. 文件清单与职责

| 文件 | 状态 | 职责 |
|------|------|------|
| `linux_compat.cpp` | 改 | 注入完整内核内 ELF 动态链接器（`linux_dynload_and_exec` + `dyn_map_image`/`dyn_parse`/`dyn_resolve`/`dyn_apply_one`/`dyn_apply` + 类型/宏/`dyn_mod_t`/`DYN_LIB_BASE`/`DYN_MAX_LIBS`）；`linux_build_stack` 加 `at_base`（AT_BASE）形参；`linux_run` 与 `sys_execve` 重执行路径接入 `linux_dynload_and_exec` |
| `usr/libc.h` | 改 | 加 `int nex_add(int a,int b);`（跨 .so 导出测试声明） |
| `usr/libc.c` | 改 | 重写为 wrapper：`#include "libc_impl.c"` + **hidden naked `_start`**（避免 libc.so 链接时 `main` 未定义冲突）；`nex_add` 实现留在此 wrapper |
| `usr/libc_impl.c` | 新建 | 全部实现（syscall 包装 / socket bridge / heap / string / memory / stdio），**无 `_start`**；供静态 guest（经 libc.c include）与 libc.so（`-fPIC` 编译）共用 |
| `usr/dynlink_crt.c` | 新建 | 动态链接 guest 的 `_start`（naked，读 argc/argv/envp 调 main） |
| `usr/linux_dynlink.c` | 新建 | 测试 guest：`main` 调 `printf` + `nex_add(2,3)`/`nex_add(10,20)`，打 `LXDL:` 标记 |
| `Makefile` | 改 | `libc.so` 规则、`linux_dynlink` 规则、`$(LINUX_SFS_IMG)` 依赖恢复 `linux_dynlink` |
| `tools/verify_linux_dynlink.py` | 新建 | 无头 QEMU 验证 harness（9 项 CHECKS） |

### 2.1 关键代码位置（linux_compat.cpp）
- 类型/宏定义：`~1758–1790`（`Elf32_Phdr/Sym/Rel`、重定位宏、`DYN_LIB_BASE=0x0A400000`、`DYN_MAX_LIBS=8`、`dyn_mod_t`）
- `dyn_map_image`：`~1810`
- `dyn_parse`：`~1837`
- `dyn_resolve`：`~1877`
- `dyn_apply_one`：`~1891`；`dyn_apply`：`~1924`
- `linux_dynload_and_exec`：`~1935`（**注意 2024 行附近 `g_guest_entry = entry_rt;` 已含 main_bias，见 §4 坑#1**）
- `linux_build_stack`：`~1598`（AT_BASE 形参在 `~1602`，auxv 填 `AT_BASE=7` 在 `~1712`、AT_ENTRY 在 `~1714`）
- 静态调用点补 `,0`：`linux_run` ~2113、`sys_execve` ~788（两处静态路径第四参数 at_base 传 0）

---

## 3. 构建环境（纯 Windows，不用 WSL）

- 统一构建入口：`tools/build_win.sh <target>`，在**真实 MSYS2 bash** 下调用：
  ```
  "C:\msys64\usr\bin\bash.exe" -lc 'cd /d/MyOS/bootloader && tools/build_win.sh textboot'
  ```
  ⚠️ WorkBuddy 自带 Git Bash 会把 `/msys64` 解析成 `E:\Program Files\Git\msys64`（不存在），导致 `make: not found`。**必须用上面这个真实 MSYS2 路径**。
- 交叉工具链：`i686-elf-g++` / `i686-elf-ld`（`C:/Users/trans/elf_tools/bin/`），由 `build_win.sh` 的 PATH 补上。
- QEMU：`D:\qemu\qemu-system-x86_64.exe`。本机 WHPX 不可用 → 所有验证一律 `-accel tcg`（内存紧用 `-m 256 -accel tcg,tb-size=128`）。
- 本机无原生 C 编译器（只有交叉编译器产出 ELF 不能直接执行）；要对 C 逻辑做确定性实测时，把逻辑 1:1 移植成 Python 用托管 python 跑。

### 3.1 构建目标
- `tools/build_win.sh textboot` → `build/os_textboot.img`（BIOS raw，含 32 位内核 + kernel64 + SFS + linux_sfs）。本 Stage 验证用这个。
- 全量 `tools/build_win.sh`（产出 `os.img`）当前可能因后台代理 WIP（见 §5 遗留）无法干净重建，先以 `textboot` 为准。

### 3.2 Makefile 关键规则（Stage 6）
`libc.so`（Makefile ~453）：
```
$(NEX_CC) $(NEX_CFLAGS) -fPIC -fno-plt -c usr/libc_impl.c -o $(BUILD)/libc_pic.o
$(NEX_LD) -m elf_i386 -shared -nostdlib --soname=libc.so --hash-style=sysv -z now -o $@ $(BUILD)/libc_pic.o
```
`linux_dynlink`（Makefile ~462）：
```
两 .o 用: -x c -m32 -ffreestanding -nostdlib -fno-stack-protector -fno-asynchronous-unwind-tables -O2 -Wall -Wextra -Iusr -fPIE -fno-plt -c
链接: $(NEX_LD) -m elf_i386 -nostdlib -e _start --no-dynamic-linker -pie -L$(LINUX_ROOT) -o $@ $(BUILD)/dlcrt.o $(BUILD)/dlink_app.o -lc
```

---

## 4. 已踩坑 & 修复（交接必看，避免重蹈覆辙）

### 坑#1（致命）：PIE 主程序入口未加 main_bias → #PF
- **现象**：`dyn: transferring control` 后立刻 `*** EXCEPTION 14 #PF ***`，`EIP=0000005B ERR=00000000 CR2=129C0010`。
- **根因**：主程序 PIE，链接期 `e_entry=0x1D0`，映射基址 `main_bias=0x08048000`，实际入口应为 `0x080481D0`。旧代码 `g_guest_entry = entry;`（裸链接地址）跳转 → 跳到未映射低址触发 #PF。同理 `AT_ENTRY` 也必须用运行时地址。
- **修复**：`uint32_t entry_rt = entry + main_bias;`，`g_guest_entry = entry_rt;` 且 `linux_build_stack` 的 `entry` 实参传 `entry_rt`。
- ⚠️ **续作提醒**：任何新增的 PIE guest 跳转点，入口/AT_ENTRY 一律 `+= main_bias`。

### 坑#2：libc.so 链接报 `undefined reference to main`
- **根因**：最初 `libc_pic.o` 来自 `libc.c`（含 `_start` 调 `main`），`-shared` 链接找不到 `main`。
- **修复**：拆分 `libc.c` → `libc_impl.c`（无 `_start`）+ `libc.c`（`#include "libc_impl.c"` + hidden naked `_start`）。

### 坑#3：`ld` 不认 `-m elf_i386` 当 gcc 驱动参数
- **现象**：`linux_dynlink` 用 `$(NEX_CC) -m elf_i386` 链接报未知选项。
- **修复**：改用 `$(NEX_LD) $(NEX_LDFLAGS)` 链接（见 §3.2）。

### 坑#4：`ld` 报 `cannot use executable file 'libc.so' as input`
- **根因**：① `-shared` 经 `i686-elf-g++` 未生效；② `ld` 的 `--soname,libc.so`（逗号）被拒。
- **修复**：改用 `$(NEX_LD) -m elf_i386 -shared ... --soname=libc.so`（**等号**）。验证 `readelf -h` 显示 DYN + SONAME=libc.so + HASH/STRTAB/SYMTAB/REL/BIND_NOW。
- ⚠️ **续作提醒**：`i686-elf-ld` 的 `--soname=` 用等号，逗号形式被拒。

### 坑#5：主程序 `.interp`/`.hash` LMA 与 `.rodata` 重叠
- **根因**：主程序原非 PIE 且带 `.interp`，链接布局冲突。
- **修复**：主程序改 PIE（`-fPIE -pie`）+ `--no-dynamic-linker`，内核算 `main_bias` 映射至 0x08048000。

### 坑#6：内核 `mods[0].base` 未初始化
- **现象**：动态主程序 RELATIVE/符号地址用垃圾 base。
- **修复**：`mods[0].base = main_bias`；`dyn_map_image(elf,sz,main_bias)`；DT 读用 `main_bias+d_vaddr`；`load_base = main_bias+(first_v-first_o)`。

### 坑#7：libc.so 内部 GLOB_DAT 解析失败刷大量 `dyn: unresolved`
- **现象**：`putchar/memcpy/malloc/nex_write/vprintf/memset/strlen/free` 全 unresolved。
- **根因**：`dyn_apply(lib, mods, nmods)` 用旧 nmods（不含 lib 自身）→ libc.so 内部 GLOB_DAT（printf→vprintf→nex_write）无法自解析。
- **修复**：改为 `dyn_apply(lib, mods, nmods + 1)` 让 lib 自解析。

---

## 5. 验证方式与结果

### 5.1 验证 harness
`tools/verify_linux_dynlink.py`（无头 QEMU `pc/256M/tcg`，**无 NIC**）：
1. 启动 QEMU → 等登录提示 → 登录 `root`/`admin`
2. 等 Shell ready → 发 `linux linux_dynlink`
3. 抓 serial 落 `build/serial_linux_dynlink.txt`，断言 9 项 CHECKS

### 5.2 9 项 CHECKS（当前全 PASS）
```
[PASS] kernel detected dynamic image (loaded libc.so)
[PASS] kernel transferred control to dynamic exe
[PASS] guest printf resolved from libc.so (printf_ok)
[PASS] guest cross-.so call resolved (resolved_sym_ok)
[PASS] nex_add(2,3)==5 via libc.so
[PASS] nex_add(10,20)==30 via libc.so
[PASS] no unresolved symbols
[PASS] no dyn errors
[PASS] guest exited cleanly
```

### 5.3 关键 serial 证据
```
dyn: loaded libc.so at 0x0A400000
dyn: transferring control to dynamic executable
LXDL: hello from dynamically-linked guest
LXDL: printf_ok
LXDL: nex_add(2,3)=5
LXDL: resolved_sym_ok
LXDL: nex_add(10,20)=30
linux: process exited
```

### 5.4 如何复跑
```bash
"C:\msys64\usr\bin\bash.exe" -lc 'cd /d/MyOS/bootloader && tools/build_win.sh textboot'
python3 tools/verify_linux_dynlink.py
```
期望输出 `VERDICT: GOOD: Stage 6 PASS`。

---

## 6. 已知遗留（非本 Stage 阻塞，待对应代理/后续）

1. **用户尚未 OK**：Stage 6 验证问答已发（2026-08-24 早），截至本文档编写时用户未回 OK。按项目纪律，用户 OK 才算收工。
2. **`mc_launcher.nex` 预构建二进制**仍带旧 syscall 号 400/401（源码已同步 410/411），需后续重编译 guest + 重打包 SFS 才完全对齐（remote-desktop 暂以 400/401 调会落到 socket 桥，属已知遗留）。
3. **64 位 PMM** 仍有 `bss end overlaps HEAP_START` 警告（`.bss` 0x4354A0 接近 0x500000，功能正常但建议后续抬 64 位 HEAP_START 或加载基址）。
4. **后台代理 WIP 可能打断构建**：工作树里 `net.cpp`/`linux_compat.cpp` 等常有未提交半成品改动（如 `net_init` 缺声明、`case` 号撞车）。临时修复（加前向声明 / 临时改系统调用号）拿到可启动镜像验证后**务必回滚**，保留代理 WIP 原状，撞车/重排决定留给对应代理。
5. **`linux_dynlink` stage-6 链接重叠**早期曾从 `$(LINUX_SFS_IMG)` 依赖临时移除，现已修好并恢复依赖。

---

## 7. 后续 Stage 建议（路线下一片，用户已授权但未做）

按用户授权路线，Stage 6 之后的推进顺序：
1. **musl 适配**：用 musl 替换手写 `libc.so`，提供完整 libc API（当前 `libc_impl.c` 只是最小子集，足够驱动 Stage 6 验证，但不足以支撑 JVM）。注意 musl 的 `ldso` 期望有真正的 `ld-linux` 或自身内嵌启动；在「内核当链接器」模型下需要裁剪 musl 的 `crt1/scrt1` 与 `ldso` 启动路径，或继续走内核内链接、只复用 musl 的 libc 实现。
2. **JVM/Hotspot 引导桩**：解释 ELF + 设置 JNI/GC 堆；Hotspot 的零汇编解释器（zero/shark）在无 SSE2 的 32 位环境下可能需要额外移植工作。
3. **Mesa/3D GL**：软件光栅（llvmpipe / swrast）或对接 NexOS 现有 gfx 半透明 blit 路径（`blend_rect`/`blend_rounded_rect`，16bpp 掩码见项目记忆）。
4. **LWJGL + 真·MC Java 版**：最终目标。

---

## 8. 项目纪律（交接必须遵守）

1. **始终用简体中文回复**。
2. **每个阶段先实测、发「验证问答」等用户 OK 才收工**——编译通过 ≠ 完成，要跑通用户视角的完整流程（无头 QEMU serial 抓标记 / 截图）。
3. **渐进式推进，不擅自跳阶段**：用户说「继续」才推下一步；确认方向 → 拿到验证报告 → 再推进。
4. **用户确认即收工**：用户说 OK/没问题后立即停测，不再多跑回归。
5. **GUI 改动优先 C#**：图形界面改 `csharp/`（NexOS.Forms + Shell 应用），尽量不动 `gui.cpp` 内核层。

---

*文档结束。续作前请先 `git status` 确认工作树无代理半成品改动阻塞构建，再跑 §5.4 复跑验证确认基线绿。*
