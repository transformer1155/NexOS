# =====================================================================
#  Makefile  -  BIOS + UEFI bootloader, C++ kernel, MKFS + SFS file systems
# =====================================================================

# ===== Toolchain =====
# On a WSL / native-Linux host we use the system toolchain directly.
# Windows-side cross compilers (e.g. /d/nexos-tc/i686-elf/bin/...) are NOT
# used from WSL -- see tools/wsl_setup.mk which provisions nasm/g++/ld/...
# on first build.  Everything below is overridable from the command line.
# Override GNU make's built-in defaults (cc/as/...) but still allow an
# explicit command-line / environment override:  make CC=clang ...
ifeq ($(origin AS),default)
  AS      := nasm
endif
ifeq ($(origin CC),default)
  CC      := g++
endif
ifeq ($(origin LD),default)
  LD      := ld
endif
# OBJCOPY / PYTHON / QEMU are NOT GNU-make built-ins, so their `origin` is
# "undefined" (never "default") when unset.  Using `ifeq ($(origin X),default)`
# therefore SKIPS the assignment and leaves them empty.  Use `?=` instead so a
# missing (or empty) value falls back to a sane default while still allowing an
# explicit command-line / environment override (make OBJCOPY=...).
OBJCOPY ?= objcopy
PYTHON  ?= python3

# =====================================================================
#  WSL / native-Linux auto-provisioning
#  When this Makefile runs inside WSL (kernel contains "microsoft"),
#  include tools/wsl_setup.mk which resolves the system toolchain and,
#  on first build, apt-get installs anything that is missing
#  (nasm, g++, binutils, gnu-efi, ovmf, qemu, mtools, xorriso, ...).
#  Override detection with:  make WSL=0 ...   (force non-WSL behaviour)
# =====================================================================
ifeq ($(origin WSL),default)
  ifneq ($(shell uname -r 2>/dev/null | grep -i -c microsoft),0)
    WSL := 1
  else
    WSL := 0
  endif
endif
ifeq ($(WSL),1)
  include tools/wsl_setup.mk
endif

BUILD   := build
IMG     := $(BUILD)/os.img
UEFI_IMG := $(BUILD)/os_uefi.img
SFS_IMG := $(BUILD)/sfs.img
SFS_DIR := sfs_files

# =====================================================================
#  QEMU executable
#  QEMU resolution:
#   - When invoked from inside WSL we use the *native* Linux qemu that
#     tools/wsl_setup.mk provisions (qemu-system-x86).  This keeps the
#     whole build loop inside WSL (no dependency on a Windows-side
#     qemu.exe).  WSLg / GTK frontend works on Windows 11 WSL.
#   - On a plain Linux host we use the system `qemu-system-x86_64`.
#   - If you really want the Windows QEMU instead, override with:
#        make run QEMU=/mnt/d/qemu/qemu-system-x86_64.exe
#  The actual provisioning (apt-get install qemu-system-x86) happens in
#  tools/wsl_setup.mk; here we just resolve a sensible default.
# =====================================================================
# QEMU is not a GNU-make built-in (origin "undefined" when unset), so the old
# `ifeq ($(origin QEMU),default)` test never matched and left QEMU empty.
# Use `?=` to fall back to qemu-system-x86_64 unless overridden.  tools/wsl_setup.mk
# refines this to a found binary on WSL; `?=` lets that override win too.
QEMU ?= qemu-system-x86_64

# ----- Flags -----
ASFLAGS_BIN := -f bin
ASFLAGS_ELF := -f elf32

CXXFLAGS := -m32 -ffreestanding -fno-exceptions -fno-rtti \
            -fno-stack-protector -fno-pic -fno-pie -fcf-protection=none \
            -fno-strict-aliasing \
            -fno-asynchronous-unwind-tables -nostdlib -O2 -Wall -Wextra

LDFLAGS  := -m elf_i386 -nostdlib -T linker.ld -z noexecstack

# ----- 64-bit (long-mode) kernel build variables -----
# CC64 lets a Windows/native build use a dedicated x86_64-elf cross compiler
# for the long-mode kernel while CC (i686-elf) builds the 32-bit kernel.
# Default to $(CC) so Linux toolchains that are multilib-capable keep working.
CC64       ?= $(CC)
CXX64FLAGS := -I. -m64 -mno-red-zone -mcmodel=kernel -ffreestanding -fno-exceptions -fno-rtti -fno-stack-protector -fno-pic -fno-pie -fcf-protection=none -fno-asynchronous-unwind-tables -nostdlib -O2 -Wall -Wextra -fpermissive -DMINIOS_HAVE_MFORMS=1 -DNexOS_HAVE_MFORMS=1
AS64FLAGS  := -f elf64
LD64       := ld
LDFLAGS64  := -m elf_x86_64 -nostdlib -T .attic64/linker64.ld -z noexecstack --unresolved-symbols=ignore-all

# ----- UEFI build variables -----
EFI_INC   := /usr/include/efi
EFI_LIB   := /usr/lib
EFI_CRT0  := $(EFI_LIB)/crt0-efi-x86_64.o
EFI_LDS   := $(EFI_LIB)/elf_x86_64_efi.lds
OVMF_CODE := /usr/share/OVMF/OVMF_CODE.fd
OVMF_VARS := /usr/share/OVMF/OVMF_VARS.fd

EFI_CFLAGS  := -I$(EFI_INC) -I$(EFI_INC)/x86_64 -DEFI_FUNCTION_WRAPPER \
               -DGNU_EFI_USE_MS_ABI -fno-stack-protector -fshort-wchar \
               -Wall -Wextra
EFI_LDFLAGS := -nostdlib -znocombreloc -T $(EFI_LDS) -shared -Bsymbolic \
               -L$(EFI_LIB) $(EFI_CRT0)

# SFS is written to disk at LBA 3488.  kernel64.bin (long-mode payload) occupies
# LBA 2048..3488 (KERNEL64_SECTORS=1440); kernel.bin occupies LBA 33..~1200;
# LBA 1200-3487 is the growth gap.  The BIOS image is 4 MiB, so the SFS
# region 3488-8191 (~2.4 MB) can hold textures.
#
# ARCHITECTURE: this tree is 32-bit only.  The long-mode kernel (kernel64.cpp,
# entry64.asm, linker64.ld, switch32to64.asm, switch64to32.asm) and the
# PE32+/amd64 browser were retired to .attic64/ -- nothing in the build,
# the disk layout or the shell references them any more.
SFS_LBA      := 3496
SFS_BYTE_OFF := $(shell echo $$(( $(SFS_LBA) * 512 )))

# Real GGUF weights bypass SFS entirely (768-sector cap) and are appended to
# the image as a raw blob: a one-sector descriptor at LBA 4095, payload from
# LBA 4096.  Override MODEL_GGUF to embed a different model, e.g.
#   make MODEL_GGUF=~/models/Qwen3-1.7B-Q4_K_M.gguf
# NOTE: the SFS image itself is ~3.2k sectors (3488..~6600), so the blob has
# to start well past it.  LBA 16384 = the 8 MiB mark, leaving the filesystem
# room to more than double before anything collides.
MODEL_HDR_LBA  := 16383
MODEL_DATA_LBA := 16384
MODEL_GGUF     ?= $(BUILD)/test_model.gguf

ISO        := $(BUILD)/os.iso
ISO_ROOT   := $(BUILD)/iso_root
HYBRID_IMG := $(BUILD)/os_hybrid.img

.PHONY: all clean run test test-ai test-net test-gguf disasm uefi uefi-run uefi-test sfs \
        bios-run bios-test \
        iso iso-run iso-run-uefi iso-test iso-test-uefi \
        hybrid hybrid-run hybrid-run-uefi hybrid-test hybrid-test-uefi \
        test-all \
        play test-sec test-f0 test-fail test-w32 test-rclick inject \
        textboot \
        csharp test-clr winhost winhost-shot

# NOTE: default build reverted to the BIOS raw image (os.img) on 2026-08-12.
# The CD/ISO path (boot_cd.asm) had an unresolved early-crash bug; the IMG
# path (boot.asm -> stage2.asm) is the verified-good baseline, so it is the
# default again until the CD path is fixed.  `make iso` still builds the ISO.
all: $(IMG)

# =====================================================================
#  BIOS boot path
# =====================================================================

# ----- Stage 1: 512-byte boot sector -----
$(BUILD)/boot.bin: boot.asm | $(BUILD)
	$(AS) $(ASFLAGS_BIN) boot.asm -o $@

# ----- Stage 2: padded to 16 KiB (32 sectors) inside the source -----
$(BUILD)/stage2.bin: stage2.asm | $(BUILD)
	$(AS) $(ASFLAGS_BIN) stage2.asm -o $@

# ----- Kernel objects -----
$(BUILD)/entry.o: entry.asm | $(BUILD)
	$(AS) $(ASFLAGS_ELF) entry.asm -o $@

$(BUILD)/kernel.o: kernel.cpp | $(BUILD)
	$(CC) $(CXXFLAGS) -c kernel.cpp -o $@

# ----- 64-bit integer division helpers (freestanding -m32 has no i386
#       multilib libgcc, so __udivdi3/__umoddi3/__divdi3/__moddi3 are
#       supplied here via shift-and-subtract) -----
$(BUILD)/divdi3.o: divdi3.c | $(BUILD)
	$(CC) $(CXXFLAGS) -c divdi3.c -o $@

$(BUILD)/ai_engine.o: ai_engine.cpp | $(BUILD)
	$(CC) $(CXXFLAGS) -c ai_engine.cpp -o $@

$(BUILD)/kb.o: kb.cpp kb.h | $(BUILD)
	$(CC) $(CXXFLAGS) -c kb.cpp -o $@

$(BUILD)/ai_plugin.o: ai_plugin.cpp ai_plugin.h | $(BUILD)
	$(CC) $(CXXFLAGS) -c ai_plugin.cpp -o $@

$(BUILD)/skill.o: skill.cpp skill.h | $(BUILD)
	$(CC) $(CXXFLAGS) -c skill.cpp -o $@

$(BUILD)/gguf.o: gguf.cpp gguf.h | $(BUILD)
	$(CC) $(CXXFLAGS) -c gguf.cpp -o $@

$(BUILD)/net.o: net.cpp | $(BUILD)
	$(CC) $(CXXFLAGS) -c net.cpp -o $@

$(BUILD)/distnet.o: distnet.cpp distnet.h | $(BUILD)
	$(CC) $(CXXFLAGS) -c distnet.cpp -o $@

$(BUILD)/gui.o: gui.cpp logo.h font_vec.h | $(BUILD)
	$(CC) $(CXXFLAGS) -c gui.cpp -o $@

# Runtime vector font rasterizer (stb_truetype.h) - 32-bit and 64-bit.
# Compiled as plain C (-x c) so its kmalloc/kfree calls use C linkage, matching
# the extern "C" definitions in kernel.cpp.  tools/vecmath supplies <math.h>.
$(BUILD)/font_vec.o: font_vec.c font_vec.h tools/stb/stb_truetype.h | $(BUILD)
	$(CC) -x c $(CXXFLAGS) -I tools/stb -I tools/vecmath -c font_vec.c -o $@

$(BUILD)/font_vec64.o: font_vec.c font_vec.h tools/stb/stb_truetype.h | $(BUILD)
	$(CC64) -x c $(CXX64FLAGS) -I tools/stb -I tools/vecmath -c font_vec.c -o $@

# 32-bit kernel uses the no-op stub (real rasterizer is 64-bit only, to stay
# within the 0x10000..0x9FC00 load window).  gui.cpp falls back to font_la.
$(BUILD)/font_vec_stub.o: font_vec_stub.c font_vec.h | $(BUILD)
	$(CC) $(CXXFLAGS) -c font_vec_stub.c -o $@

$(BUILD)/addrman.o: addrman.cpp addrman.h | $(BUILD)
	$(CC) $(CXXFLAGS) -c addrman.cpp -o $@

$(BUILD)/addrman64.o: addrman.cpp addrman.h | $(BUILD)
	$(CC64) $(CXX64FLAGS) -c addrman.cpp -o $@

$(BUILD)/winloader.o: winloader.cpp | $(BUILD)
	$(CC) $(CXXFLAGS) -c winloader.cpp -o $@

$(BUILD)/win32.o: win32.cpp win32.h | $(BUILD)
	$(CC) $(CXXFLAGS) -c win32.cpp -o $@

# ----- Linux binary-compat shim (Wine-on-NexOS Milestone 0) -----
$(BUILD)/linux_compat.o: linux_compat.cpp linux_compat.h setjmp.h syscall.h | $(BUILD)
	$(CC) $(CXXFLAGS) -c linux_compat.cpp -o $@

# ----- Foundation 0: ring-3 GDT/TSS + syscall ABI + process table -----
$(BUILD)/gdt.o: gdt.cpp gdt.h | $(BUILD)
	$(CC) $(CXXFLAGS) -c gdt.cpp -o $@

$(BUILD)/syscall.o: syscall.cpp syscall.h proc.h setjmp.h vfs.h mm.h | $(BUILD)
	$(CC) $(CXXFLAGS) -c syscall.cpp -o $@

$(BUILD)/proc.o: proc.cpp proc.h | $(BUILD)
	$(CC) $(CXXFLAGS) -c proc.cpp -o $@

$(BUILD)/perm.o: perm.cpp perm.h proc.h | $(BUILD)
	$(CC) $(CXXFLAGS) -c perm.cpp -o $@

$(BUILD)/vfs.o: vfs.cpp vfs.h proc.h perm.h | $(BUILD)
	$(CC) $(CXXFLAGS) -c vfs.cpp -o $@

# ----- MiniCLR: CIL interpreter for real Roslyn-compiled C# apps -----
$(BUILD)/clr.o: clr.cpp clr.h | $(BUILD)
	$(CC) $(CXXFLAGS) -c clr.cpp -o $@

# ----- NexOS.Forms: native host for the managed (C#) GUI shell -----
# NOTE: -fno-optimize-sibling-calls is REQUIRED here. The icall wrapper
# functions (h_mem_total, h_pages_*, h_heap_*, ...) forward to function
# pointers stored in g_h. Under -O2, GCC turns `return (int32_t)g_h.f();`
# into a tail-call `jmp *funcptr`, and that indirect jump reads an
# unreliable pointer in this freestanding -m32 build, causing #PF
# (EIP=1, CR2=0xFFFFFFEC). Disabling sibling-call optimisation forces a
# proper `call *funcptr` and eliminates the crash for every wrapper.
$(BUILD)/mforms.o: mforms.cpp mforms.h clr.h | $(BUILD)
	$(CC) $(CXXFLAGS) -fno-optimize-sibling-calls -c mforms.cpp -o $@

# ----- Link kernel ELF (entry.o first => _start at image offset 0) -----
$(BUILD)/kernel.elf: $(BUILD)/entry.o $(BUILD)/switch32to64.o $(BUILD)/kernel.o $(BUILD)/divdi3.o $(BUILD)/ai_engine.o $(BUILD)/ai_plugin.o $(BUILD)/kb.o $(BUILD)/skill.o $(BUILD)/gguf.o $(BUILD)/net.o $(BUILD)/distnet.o $(BUILD)/gui.o $(BUILD)/font_vec_stub.o $(BUILD)/addrman.o $(BUILD)/winloader.o $(BUILD)/win32.o $(BUILD)/linux_compat.o $(BUILD)/gdt.o $(BUILD)/syscall.o $(BUILD)/proc.o $(BUILD)/vfs.o $(BUILD)/perm.o $(BUILD)/clr.o $(BUILD)/mforms.o linker.ld | $(BUILD)
	$(LD) $(LDFLAGS) -o $@ $(BUILD)/entry.o $(BUILD)/switch32to64.o $(BUILD)/kernel.o $(BUILD)/divdi3.o $(BUILD)/ai_engine.o $(BUILD)/ai_plugin.o $(BUILD)/kb.o $(BUILD)/skill.o $(BUILD)/gguf.o $(BUILD)/net.o $(BUILD)/distnet.o $(BUILD)/gui.o $(BUILD)/font_vec_stub.o $(BUILD)/addrman.o $(BUILD)/winloader.o $(BUILD)/win32.o $(BUILD)/linux_compat.o $(BUILD)/gdt.o $(BUILD)/syscall.o $(BUILD)/proc.o $(BUILD)/vfs.o $(BUILD)/perm.o $(BUILD)/clr.o $(BUILD)/mforms.o

# ----- Extract flat kernel binary -----
$(BUILD)/kernel.bin: $(BUILD)/kernel.elf | $(BUILD)
	$(OBJCOPY) -O binary $< $@
	@sz=$$(stat -c%s $@); \
	 if [ $$sz -gt 589824 ]; then \
	   echo "*** ERROR: kernel.bin is $$sz bytes (> 576 KiB)."; \
	   echo "***        stage2.asm loads KERNEL_SECTORS=1152 sectors (576 KiB) and the"; \
	   echo "***        kernel loads at 0x10000, so it must end <= 0xA0000 (VGA MMIO hole)."; \
	   echo "***        The kernel would be silently truncated -- refusing to build."; \
	   rm -f $@; exit 1; \
	 elif [ $$sz -gt 530841 ]; then \
	   echo "    [warn] kernel.bin $$sz bytes: >90% of the 576 KiB load window"; \
	 fi

# =====================================================================
#  SFS (Simple File System) image generation
#  Packs files from sfs_files/ into a read-only FS image.
#  tools/tex_pack.py converts assets/*.png into sfs_files/tex_*.tex
#  (procedural fallback if an asset is missing) so the BIOS disk gets
#  detailed textures.  The UEFI disk keeps a texture-free SFS because
#  its SFS slot (LBA 800) is only ~224 sectors before the ESP (LBA 1024).
# =====================================================================
$(SFS_IMG): $(wildcard $(SFS_DIR)/*) tools/sfs_gen.py tools/tex_pack.py $(SFS_DIR)/userdemo $(SFS_DIR)/hello.mex $(SFS_DIR)/shell.mex $(SFS_DIR)/hello.nex | $(BUILD)
	$(PYTHON) tools/tex_pack.py
	$(PYTHON) tools/sfs_gen.py $(SFS_DIR) $@

$(BUILD)/sfs_uefi.img: $(wildcard $(SFS_DIR)/*) tools/sfs_gen.py $(SFS_IMG) | $(BUILD)
	$(PYTHON) tools/sfs_gen.py $(SFS_DIR) $@

# =====================================================================
#  Independent Linux user-space partition
#  Packs linux_root/ into its own SFS volume, written to the BIOS disk
#  at LINUX_SFS_LBA (after the main SFS volume, before the GGUF blob).
#  The kernel mounts it as a second SFS and `linux <file>` reads ELF
#  binaries from here first.
# =====================================================================
LINUX_ROOT     := linux_root
LINUX_SFS_IMG  := $(BUILD)/linux_sfs.img
# 12288 (6 MiB mark): main SFS grows past 8704 on the BIOS image, so 8704
# would overlap it and clobber zfont.bin's tail.  12288 leaves ~2.8k sectors
# of free space in the main SFS (usable by Sfs::create) before the Linux vol.
LINUX_SFS_LBA  := 3800

# Stage 6 dynamic-link test guest + libc.so are both packed into the Linux
# SFS volume so `linux linux_dynlink` can load libc.so from there.
$(LINUX_SFS_IMG): $(LINUX_ROOT)/python $(LINUX_ROOT)/linux_argv $(LINUX_ROOT)/linux_net $(LINUX_ROOT)/linux_dynlink $(LINUX_ROOT)/libc.so $(LINUX_ROOT)/gnu_dynlink $(LINUX_ROOT)/libgnu.so $(wildcard $(LINUX_ROOT)/*) tools/sfs_gen.py | $(BUILD)
	$(PYTHON) tools/sfs_gen.py $(LINUX_ROOT) $@

sfs: $(SFS_IMG)

# =====================================================================
#  MiniPE apps  (real Windows PE binaries, cross-compiled with mingw-w64)
#
#  These are genuine PE images: MZ/PE header, import table, native entry
#  point.  They reach the screen only through the Win32 API exported by
#  win32.cpp, and the kernel loads them through the very same winapp path
#  as any other PE -- there is no special casing.
#
#  A PE names exactly one machine type in its COFF header, so the browser
#  ships as two images rather than one fat binary:
#      chrome.exe    amd64 PE32+  -> win64_run, 64-bit kernel
#      iexplore.exe  i386  PE32   -> win32_run, 32-bit kernel
#  launch_browser_pe() in gui.cpp tries Chrome first and falls back to
#  Internet Explorer, so the Browser icon works on either kernel.
#
#  i386 needs --entry=_PeMain (leading underscore) and --dynamicbase: the
#  code is not position independent, so the loader must have a .reloc
#  section to rebase onto its kmalloc'd image.  amd64 is RIP-relative and
#  needs no fixups at all.
#
#  Prebuilt binaries are committed under sfs_files/, so this target is
#  only needed after editing winpe/*.c:  make winpe
# =====================================================================
MINGW64 ?= x86_64-w64-mingw32-gcc
MINGW32 ?= i686-w64-mingw32-gcc
PEFLAGS  = -O2 -nostdlib -ffreestanding -fno-asynchronous-unwind-tables \
           -fno-ident -Wl,--subsystem,windows

#  The kernel is 32-bit only, so the amd64 half of this pair (chrome.exe)
#  has moved to .attic64/ and is no longer built.  iexplore.exe is the
#  browser, and MINGW64 is kept only so the attic can be rebuilt by hand.
.PHONY: winpe
winpe: winpe/iexplore.exe winpe/ntbrowser.exe winpe/notepad.exe
	$(call ensure-pkg, i686-w64-mingw32-gcc, $(PKG_MINGW))
	cp winpe/iexplore.exe $(SFS_DIR)/iexplore.exe
	$(PYTHON) tools/pe_gap.py winpe/iexplore.exe
	cp winpe/ntbrowser.exe $(SFS_DIR)/ntbrowser.exe
	$(PYTHON) tools/pe_gap.py winpe/ntbrowser.exe
	cp winpe/notepad.exe $(SFS_DIR)/notepad.exe
	$(PYTHON) tools/pe_gap.py winpe/notepad.exe

winpe/iexplore.exe: winpe/iexplore.c winpe/minipe.h
	$(MINGW32) $(PEFLAGS) -Wl,--entry=_PeMain -Wl,--dynamicbase \
	    -o $@ $< -lkernel32 -luser32 -lgdi32

# notepad.exe: stage-2 Win32 demo -- a real 32-bit GUI program with a
# white "edit" area and two BUTTON controls (Echo/Clear).  Proves the
# user32/gdi32 L1->L2 jump: genuine windows, controls and message loop.
winpe/notepad.exe: winpe/notepad.c
	$(MINGW32) $(PEFLAGS) -Wl,--entry=_WinMainCRTStartup -Wl,--dynamicbase \
	    -o $@ $< -lkernel32 -luser32 -lgdi32

# ntbrowser.exe: the real networked NT browser (fetches via minios.dll
# MiniHttpGet) + the mini integer JS subset (minijs.c) for <script> blocks.
winpe/ntbrowser.exe: winpe/ntbrowser.c winpe/minijs.c winpe/minijs.h winpe/minipe.h
	$(MINGW32) $(PEFLAGS) -Wl,--entry=_PeMain -Wl,--dynamicbase \
	    -o $@ winpe/ntbrowser.c winpe/minijs.c -lkernel32 -luser32 -lgdi32

# =====================================================================
#  C# managed apps  (real Roslyn compile -> flat .mex image for MiniCLR)
#  Requires the .NET SDK.  Assemblies are built with /nostdlib against
#  csharp/NexOS.Core so they contain zero AssemblyRef rows, letting
#  tools/mex_pack.py resolve every metadata token inside one file.
# =====================================================================
#  The SDK may live on the Windows side when building from WSL, in which
#  case dotnet.exe is reachable through the interop PATH.
CS_CORE  := csharp/NexOS.Core/Corelib.cs csharp/NexOS.Core/Sys.cs

$(SFS_DIR)/hello.mex: csharp/apps/Hello/Program.cs csharp/apps/Hello/Hello.csproj $(CS_CORE) tools/mex_pack.py
	@if command -v dotnet >/dev/null 2>&1; then DN=dotnet; \
	 elif command -v dotnet.exe >/dev/null 2>&1; then DN=dotnet.exe; \
	 else echo "!! .NET SDK not found - keeping existing $@"; exit 0; fi; \
	 "$$DN" build csharp/apps/Hello/Hello.csproj -c Release -v quiet --nologo
	$(PYTHON) tools/mex_pack.py csharp/apps/Hello/bin/Release/Hello.dll $@
	@echo "==> C# app packed: $@ ($$(stat -c%s $@) bytes)"

# The resident GUI shell: NexOS.Forms toolkit + every built-in app +
# desktop, compiled to one /nostdlib assembly and flattened to shell.mex.
# Entry point is Shell::Init; mforms.cpp drives the rest by name.
SHELL_SRC := csharp/apps/Shell/Shell.cs csharp/apps/Shell/Apps.cs \
             csharp/apps/Shell/Desktop.cs csharp/apps/Shell/Browser.cs \
             csharp/apps/Shell/AiSetup.cs csharp/apps/Shell/AiAgent.cs \
             csharp/apps/Shell/Login.cs \
             csharp/apps/Shell/Popup.cs \
             csharp/apps/Shell/Lang.cs \
             csharp/apps/Shell/Demo.cs \
             csharp/NexOS.Forms/Forms.cs csharp/NexOS.Forms/Voice.cs \
             csharp/apps/Shell/Shell.csproj
$(SFS_DIR)/shell.mex: $(SHELL_SRC) $(CS_CORE) tools/mex_pack.py
	@if command -v dotnet >/dev/null 2>&1; then DN=dotnet; \
	 elif command -v dotnet.exe >/dev/null 2>&1; then DN=dotnet.exe; \
	 else echo "!! .NET SDK not found - keeping existing $@"; exit 0; fi; \
	 "$$DN" build csharp/apps/Shell/Shell.csproj -c Release -v quiet --nologo
	$(PYTHON) tools/mex_pack.py csharp/apps/Shell/bin/Release/Shell.dll $@ "NexOS.Forms.Shell::Init"
	@echo "==> C# shell packed: $@ ($$(stat -c%s $@) bytes)"

csharp: $(SFS_DIR)/hello.mex $(SFS_DIR)/shell.mex
	$(call ensure-pkg, dotnet, $(PKG_DOTNET))

# ----- Windows harness: run the very same C# shell on .NET ------------
#  csharp/winhost compiles Shell.cs / Apps.cs / Desktop.cs / Forms.cs /
#  Sys.cs unchanged (Forms.cs and Sys.cs hide their [InternalCall] stubs
#  behind "#if !WINHOST") and binds them to GDI+ instead of the kernel.
#  Iterating on the desktop no longer needs a VM boot.
#
#    make winhost        launch the interactive window
#    make winhost-shot   render one frame to build/winhost_*.png
WINHOST_PROJ := csharp/winhost/WinHost.csproj
WINHOST_SRC  := $(WINHOST_PROJ) csharp/winhost/Program.cs csharp/winhost/Backend.cs \
                csharp/winhost/ShellForm.cs csharp/winhost/ShellForm.Designer.cs \
                csharp/winhost/ShellForm.resx \
                $(SHELL_SRC) csharp/NexOS.Core/Sys.cs

define FIND_DOTNET
if command -v dotnet >/dev/null 2>&1 && dotnet --list-sdks >/dev/null 2>&1; then DN=dotnet; \
elif command -v dotnet.exe >/dev/null 2>&1; then DN=dotnet.exe; \
elif [ -x "/mnt/c/Program Files/dotnet/dotnet.exe" ]; then DN="/mnt/c/Program Files/dotnet/dotnet.exe"; \
else echo "!! .NET SDK not found"; exit 1; fi
endef

winhost: $(WINHOST_SRC)
	@$(FIND_DOTNET); \
	 echo "==> launching the C# shell on Windows (.NET WinForms)"; \
	 "$$DN" run --project $(WINHOST_PROJ) -v quiet --nologo

winhost-shot: $(WINHOST_SRC) | $(BUILD)
	@$(FIND_DOTNET); \
	 "$$DN" run --project $(WINHOST_PROJ) -v quiet --nologo -- \
	     --shot $(BUILD)/winhost_desktop.png 700; \
	 "$$DN" run --project $(WINHOST_PROJ) -v quiet --nologo -- \
	     --shot $(BUILD)/winhost_apps.png 700 0,1,2,5
	@echo "==> frames written to $(BUILD)/winhost_*.png"

# ----- ring-3 user demo (flat binary) for Foundation 0 test -----
# Linked at USER_BASE (0x04000000) so the identity-mapped user region
# executes it directly. Packed into SFS; the `user` command loads it.
USERDEMO_TEXT := 0x04000000
$(SFS_DIR)/userdemo: tools/userdemo.S | $(SFS_DIR)
	$(AS) $(ASFLAGS_ELF) tools/userdemo.S -o $(BUILD)/userdemo.o
	$(LD) -m elf_i386 -static -Ttext=$(USERDEMO_TEXT) -o $(BUILD)/userdemo.elf $(BUILD)/userdemo.o
	$(OBJCOPY) -O binary $(BUILD)/userdemo.elf $@

# =====================================================================
#  NexOS native user programs (.nex) -- P1 basic runtime demo
#  Freestanding ELF32 linked at NEX_TEXT (0x08048000) -- the guest load
#  region INSIDE the 64-256 MiB PG_USER identity map that linux_run()
#  (linux_compat.cpp) lays out for the ELF image / brk / strings / stack /
#  mmap.  This keeps the guest clear of the 32-bit kernel boot stack, which
#  linker.ld reserves at 0x1800000 (24 MiB) -- loading a guest at the old
#  0x01800000 base overwrote the live kernel stack and corrupted the loader.
#  A minimal libc (usr/libc.c) supplies printf / malloc / free / string.
NEX_CC      ?= $(CC)
NEX_LD      ?= $(LD)
NEX_TEXT    := 0x08048000
NEX_CFLAGS  := -x c -m32 -ffreestanding -nostdlib -fno-stack-protector -fno-pic \
               -fno-pie -fno-asynchronous-unwind-tables -O2 -Wall -Wextra -Iusr
NEX_LDFLAGS := -m elf_i386 -nostdlib -Ttext=$(NEX_TEXT) -e _start

.PHONY: nex
nex: $(SFS_DIR)/hello.nex

$(SFS_DIR)/hello.nex: usr/libc.c usr/libc.h usr/hello.c | $(SFS_DIR)
	$(NEX_CC) $(NEX_CFLAGS) -c usr/libc.c -o $(BUILD)/nex_libc.o
	$(NEX_CC) $(NEX_CFLAGS) -c usr/hello.c -o $(BUILD)/nex_hello.o
	$(NEX_LD) $(NEX_LDFLAGS) $(BUILD)/nex_libc.o $(BUILD)/nex_hello.o -o $@
	@echo "==> NexOS app packed: $@ ($$(stat -c%s $@) bytes)"

# Linux-compat Python-subset interpreter (Milestone: "AI writes Hello world").
# Built as a freestanding ELF32 (same int 0x80 runtime as hello.nex) and
# dropped into the independent Linux partition (linux_root/) so `linux python
# hello_demo.py` loads it from there and opens its script from the main SFS.
$(LINUX_ROOT)/python: usr/libc.c usr/libc.h usr/python.c | $(LINUX_ROOT)
	$(NEX_CC) $(NEX_CFLAGS) -c usr/libc.c -o $(BUILD)/py_libc.o
	$(NEX_CC) $(NEX_CFLAGS) -c usr/python.c -o $(BUILD)/py_python.o
	$(NEX_LD) $(NEX_LDFLAGS) $(BUILD)/py_libc.o $(BUILD)/py_python.o -o $@
	@echo "==> Linux Python interpreter packed: $@ ($$(stat -c%s $@) bytes)"

# Stage 4 argv/envp transparency test (freestanding ELF32). Drops into the
# independent Linux partition so `linux linux_argv ...` loads it from there.
$(LINUX_ROOT)/linux_argv: usr/libc.c usr/libc.h usr/linux_argv.c | $(LINUX_ROOT)
	$(NEX_CC) $(NEX_CFLAGS) -c usr/libc.c -o $(BUILD)/argv_libc.o
	$(NEX_CC) $(NEX_CFLAGS) -c usr/linux_argv.c -o $(BUILD)/argv_linux_argv.o
	$(NEX_LD) $(NEX_LDFLAGS) $(BUILD)/argv_libc.o $(BUILD)/argv_linux_argv.o -o $@
	@echo "==> Linux argv/envp test packed: $@ ($$(stat -c%s $@) bytes)"

# Stage 5 Linux socket bridge test (freestanding ELF32). Connects to a host
# TCP echo server (10.0.2.2:18099 under QEMU user-net) via the 400-404
# socket syscalls and verifies the round-trip echo.
$(LINUX_ROOT)/linux_net: usr/libc.c usr/libc.h usr/linux_net.c | $(LINUX_ROOT)
	$(NEX_CC) $(NEX_CFLAGS) -c usr/libc.c -o $(BUILD)/lnxnet_libc.o
	$(NEX_CC) $(NEX_CFLAGS) -c usr/linux_net.c -o $(BUILD)/lnxnet_app.o
	$(NEX_LD) $(NEX_LDFLAGS) $(BUILD)/lnxnet_libc.o $(BUILD)/lnxnet_app.o -o $@
	@echo "==> Linux socket test packed: $@ ($$(stat -c%s $@) bytes)"

# Stage 6 shared C runtime (libc.so) + dynamically-linked test guest.
# libc.so is built -fPIC -shared (soname libc.so, sysv hash, BIND_NOW) so the
# in-kernel ELF dynamic linker (linux_compat.cpp) can map it and resolve the
# main's GLOB_DAT/PLT references against it.  _start is hidden in libc.c so
# the guest can supply its own _start (dynlink_crt.c) without a clash.
$(LINUX_ROOT)/libc.so: usr/libc_impl.c usr/libc.h | $(LINUX_ROOT)
	$(NEX_CC) $(NEX_CFLAGS) -fPIC -fno-plt -c usr/libc_impl.c -o $(BUILD)/libc_pic.o
	$(NEX_LD) -m elf_i386 -shared -nostdlib --soname=libc.so \
	          --hash-style=sysv -z now -o $@ $(BUILD)/libc_pic.o
	@echo "==> libc.so packed: $@ ($$(stat -c%s $@) bytes)"

# Dynamically-linked guest: PIE main that DT_NEEDED=libc.so.  Calls
# printf / nex_add through the GOT; the kernel linker loads it at 0x08048000
# and resolves the R_386_GLOB_DAT references against libc.so.
$(LINUX_ROOT)/linux_dynlink: usr/dynlink_crt.c usr/linux_dynlink.c usr/libc.h \
                             $(LINUX_ROOT)/libc.so | $(LINUX_ROOT)
	$(NEX_CC) -x c -m32 -ffreestanding -nostdlib -fno-stack-protector \
	          -fno-asynchronous-unwind-tables -O2 -Wall -Wextra -Iusr \
	          -fPIE -fno-plt -c usr/dynlink_crt.c -o $(BUILD)/dlcrt.o
	$(NEX_CC) -x c -m32 -ffreestanding -nostdlib -fno-stack-protector \
	          -fno-asynchronous-unwind-tables -O2 -Wall -Wextra -Iusr \
	          -fPIE -fno-plt -c usr/linux_dynlink.c -o $(BUILD)/dlink_app.o
	$(NEX_LD) -m elf_i386 -nostdlib -e _start --no-dynamic-linker -pie \
	          -L$(LINUX_ROOT) -o $@ $(BUILD)/dlcrt.o $(BUILD)/dlink_app.o -lc
	@echo "==> Linux dynamic-link test packed: $@ ($$(stat -c%s $@) bytes)"

# Stage 7 GNU_HASH test: a shared lib built with --hash-style=gnu (no SysV hash)
# plus a PIE guest that DT_NEEDED it.  Exercises the kernel's DT_GNU_HASH symbol
# lookup path (bloom filter + bucket chain).  Reuses the same guest runtime as
# the Stage 6 test but against a gnu-hash-only .so to prove the new code path.
$(LINUX_ROOT)/libgnu.so: usr/libc_impl.c usr/libc.h | $(LINUX_ROOT)
	$(NEX_CC) $(NEX_CFLAGS) -fPIC -fno-plt -c usr/libc_impl.c -o $(BUILD)/libgnu_pic.o
	$(NEX_LD) -m elf_i386 -shared -nostdlib --soname=libgnu.so \
	          --hash-style=gnu -z now -o $@ $(BUILD)/libgnu_pic.o
	@echo "==> libgnu.so (gnu hash) packed: $@ ($$(stat -c%s $@) bytes)"

$(LINUX_ROOT)/gnu_dynlink: usr/dynlink_crt.c usr/linux_dynlink.c usr/libc.h \
                            $(LINUX_ROOT)/libgnu.so | $(LINUX_ROOT)
	$(NEX_CC) -x c -m32 -ffreestanding -nostdlib -fno-stack-protector \
	          -fno-asynchronous-unwind-tables -O2 -Wall -Wextra -Iusr \
	          -fPIE -fno-plt -c usr/dynlink_crt.c -o $(BUILD)/gnudlcrt.o
	$(NEX_CC) -x c -m32 -ffreestanding -nostdlib -fno-stack-protector \
	          -fno-asynchronous-unwind-tables -O2 -Wall -Wextra -Iusr \
	          -fPIE -fno-plt -c usr/linux_dynlink.c -o $(BUILD)/gnudlink_app.o
	$(NEX_LD) -m elf_i386 -nostdlib -e _start --no-dynamic-linker -pie \
	          -L$(LINUX_ROOT) -o $@ $(BUILD)/gnudlcrt.o $(BUILD)/gnudlink_app.o -lgnu
	@echo "==> Linux GNU_HASH dynamic-link test packed: $@ ($$(stat -c%s $@) bytes)"

# =====================================================================
#  Disk image assembly
#  boot + stage2 + kernel (32-bit) + SFS at LBA 3488, padded to 4 MiB
# =====================================================================
$(IMG): $(BUILD)/boot.bin $(BUILD)/stage2.bin $(BUILD)/kernel.bin $(BUILD)/kernel64.bin $(SFS_IMG) $(LINUX_SFS_IMG) | $(BUILD)
	cat $(BUILD)/boot.bin $(BUILD)/stage2.bin $(BUILD)/kernel.bin > $@
	truncate -s 4M $@
	# GUARD: kernel64.bin lives in the gap between LBA 2048 and the SFS at
	# LBA $(SFS_LBA), and the 32-bit loader stages exactly KERNEL64_SECTORS
	# sectors of it (kernel.cpp).  Outgrow either bound and the failure is
	# SILENT: dd overwrites the filesystem, and/or the loader hands a
	# truncated image to switch_to_64bit() which triple-faults.  That is the
	# same "image grew into a hard-coded offset" class of bug as the 0x90000
	# boot-stack collision, so make it a build error instead of a mystery.
	@k64sz=$$(stat -c%s $(BUILD)/kernel64.bin); \
	 secs=$$(sed -n 's/^#define[[:space:]]*KERNEL64_SECTORS[[:space:]]*\([0-9]*\).*/\1/p' kernel.cpp | head -1); \
	 gap=$$(( ($(SFS_LBA) - 2048) * 512 )); \
	 lim=$$(( $$secs * 512 )); \
	 if [ $$lim -gt $$gap ]; then lim=$$gap; fi; \
	 if [ $$k64sz -gt $$lim ]; then \
	   echo "*** ERROR: kernel64.bin is $$k64sz bytes but only $$lim bytes are reachable."; \
	   echo "***        KERNEL64_SECTORS=$$secs (kernel.cpp) => $$(( $$secs * 512 )) bytes;"; \
	   echo "***        LBA 2048..$(SFS_LBA) gap => $$gap bytes."; \
	   echo "***        Raise KERNEL64_SECTORS, and if it now exceeds the gap"; \
	   echo "***        also push SFS_LBA (Makefile) and stage2/boot_cd out of the way."; \
	   exit 1; \
	 fi; \
	 echo "    K64 fit:  $$k64sz / $$lim bytes ($$(( ($$lim - $$k64sz) / 512 )) spare sectors)"
	# Write the 64-bit (long-mode) kernel at LBA 2048 so the 32-bit
	# `switch` command can load it and transition to long mode.
	dd if=$(BUILD)/kernel64.bin of=$@ bs=512 seek=2048 conv=notrunc 2>/dev/null
	# Write SFS image at LBA 3488
	dd if=$(SFS_IMG) of=$@ bs=512 seek=$(SFS_LBA) conv=notrunc 2>/dev/null
	# Write the independent Linux user-space partition after the main SFS
	dd if=$(LINUX_SFS_IMG) of=$@ bs=512 seek=$(LINUX_SFS_LBA) conv=notrunc 2>/dev/null
	@echo "==> BIOS Image built: $(IMG) ($$(stat -c%s $@) bytes)"
	@echo "    Kernel32: $$(stat -c%s $(BUILD)/kernel.bin) bytes"
	@echo "    Kernel64: $$(stat -c%s $(BUILD)/kernel64.bin) bytes at LBA 2048"
	@echo "    SFS:      $$(stat -c%s $(SFS_IMG)) bytes at LBA $(SFS_LBA)"
	@echo "    Linux FS: $$(stat -c%s $(LINUX_SFS_IMG)) bytes at LBA $(LINUX_SFS_LBA)"
	@# Real GGUF weights are far larger than SFS can hold, so they go on the
	@# disk as a bare blob after the filesystem (see tools/embed_model.py).
	@if [ -f "$(MODEL_GGUF)" ]; then python3 tools/embed_model.py $@ $(MODEL_GGUF) $(MODEL_HDR_LBA) $(MODEL_DATA_LBA); \
	 else echo "    Model:    (none - set MODEL_GGUF=path/to/model.gguf)"; fi

# =====================================================================
#  UEFI boot path
# =====================================================================

# ----- Compile UEFI bootloader C -----
$(BUILD)/bootuefi.o: uefi/bootuefi.c | $(BUILD)
	@if [ ! -e "$(EFI_CRT0)" ]; then \
	  echo "[wsl] gnu-efi not installed -- installing $(PKG_GNUEFI) ..."; \
	  sudo apt-get update -qq && sudo apt-get install -y -qq $(PKG_GNUEFI) || \
	    { echo "!! failed; run: sudo apt-get install $(PKG_GNUEFI)"; exit 1; }; \
	fi
	$(CC) -m64 $(EFI_CFLAGS) -I. -c $< -o $@

# ----- Assemble UEFI mode-transition stub -----
$(BUILD)/enter_kernel_uefi.o: uefi/enter_kernel.S | $(BUILD)
	$(CC) -m64 -c $< -o $@

# ----- Assemble RIP-relative embedded kernel accessor (avoids PE32+ relocation issues) -----
$(BUILD)/get_embedded_uefi.o: uefi/get_embedded.S | $(BUILD)
	$(CC) -m64 -c $< -o $@

# ----- Generate relocatable object with kernel.bin embedded (avoids OVMF FAT12 bug) -----
$(BUILD)/kernel_blob.o: $(BUILD)/kernel.bin | $(BUILD)
	cp $< $(BUILD)/kernel.blob
	$(LD) -r -b binary -o $@ $(BUILD)/kernel.blob

# ----- Link UEFI shared object (includes embedded kernel blob) -----
$(BUILD)/bootx64.so: $(BUILD)/bootuefi.o $(BUILD)/enter_kernel_uefi.o $(BUILD)/get_embedded_uefi.o $(BUILD)/kernel_blob.o | $(BUILD)
	$(LD) $(EFI_LDFLAGS) $^ -o $@ -lefi -lgnuefi

# ----- Convert to PE32+ EFI application -----
# NOTE: objcopy's --target=efi-app-x86-64 does NOT properly convert ELF
# .rela relocations to PE32+ .reloc base relocations (tested with
# binutils 2.38).  We use gen_reloc.py to manually generate the .reloc
# section from the ELF .rela entries and patch it into the PE32+ binary.
$(BUILD)/BOOTX64.EFI: $(BUILD)/bootx64.so tools/gen_reloc.py | $(BUILD)
	$(OBJCOPY) -j .text -j .sdata -j .data -j .dynamic \
	           -j .dynsym -j .dynstr \
	           --target=efi-app-x86_64 $< $@
	$(PYTHON) tools/gen_reloc.py $(BUILD)/bootx64.so $@

# ----- Build ESP (EFI System Partition) image with bootloader + kernels -----
# Uses FAT16 format (16 MiB) for El Torito CD boot (removable media = FAT12/16 OK).
# boot-load-size = 32768 (16MB / 512) fits in 16-bit El Torito field.
# VirtualBox UEFI: FAT16 + non-zero boot-load-size + ISO9660 fallback = compatible.
$(BUILD)/esp.img: $(BUILD)/BOOTX64.EFI $(BUILD)/kernel.bin $(SFS_IMG) | $(BUILD)
	$(call ensure-pkg, mformat, $(PKG_MTOOLS))
	dd if=/dev/zero of=$@ bs=1M count=16 2>/dev/null
	$(MFORMAT) -i $@ ::
	$(MMD)     -i $@ ::/EFI
	$(MMD)     -i $@ ::/EFI/BOOT
	$(MCOPY)   -i $@ $(BUILD)/BOOTX64.EFI ::/EFI/BOOT/BOOTX64.EFI
	$(MCOPY)   -i $@ $(BUILD)/kernel.bin   ::/kernel.bin
	@echo "==> ESP image built: $(BUILD)/esp.img ($$(stat -c%s $@) bytes, FAT16)"

# ----- Combined UEFI disk image (GPT + ESP + SFS) -----
# Uses GPT partition table so OVMF/VirtualBox can find the ESP.
# Layout (must mirror make_gpt_uefi.py):
#   LBA 0:      Protective MBR
#   LBA 1:      GPT header
#   LBA 2-33:   GPT partition entry array
#   LBA 34-2047: Unused (gap)
#   LBA 2048:   kernel64.bin (raw; read by the 32-bit kernel's `switch64`)
#   LBA 3328:   SFS image (raw; probed by Sfs::init)
#   LBA 8192+:  ESP partition (FAT16, contains BOOTX64.EFI at /EFI/BOOT/BOOTX64.EFI)
$(UEFI_IMG): $(BUILD)/esp.img $(BUILD)/sfs_uefi.img $(BUILD)/kernel64.bin tools/make_gpt_uefi.py | $(BUILD)
	$(PYTHON) tools/make_gpt_uefi.py $(BUILD)/esp.img $@ $(BUILD)/kernel64.bin $(BUILD)/sfs_uefi.img
	@echo "==> UEFI Image built: $(UEFI_IMG) ($$(stat -c%s $@) bytes)"

# ----- Top-level UEFI build target -----
uefi: $(UEFI_IMG)

# =====================================================================
#  UEFI diagnostic build (white-line hunter)
#  Isolated from the normal `uefi` target.  Compiles gui.cpp with -DFB_DIAG
#  (adds gui_fb_diag() + fb_diag_probe() that run at gui_enter and halt) and
#  bootuefi.c with -DFB_DIAG (adds the [GOPF] serial log).  The diag kernel is
#  linked with gui_diag.o in place of gui.o, temporarily substituted into the
#  embedded kernel blob, then the normal kernel.blob/kernel_blob.o are restored
#  so the `uefi` target is never touched.
#  Output: build/os_uefi_diag.img (auto-boots as BOOTX64.EFI on real hw).
#  Use:   make uefi-diag [FB_TEST=1|2|3]
# =====================================================================
UEFI_DIAG_IMG := $(BUILD)/os_uefi_diag.img

DIAG_CXX := $(CC) $(CXXFLAGS) -DFB_DIAG
ifdef FB_TEST
DIAG_CXX += -DFB_TEST=$(FB_TEST)
endif

$(BUILD)/gui_diag.o: gui.cpp | $(BUILD)
	$(DIAG_CXX) -c gui.cpp -o $@

$(BUILD)/bootuefi_diag.o: uefi/bootuefi.c | $(BUILD)
	$(CC) $(EFI_CFLAGS) -DFB_DIAG -c uefi/bootuefi.c -o $@

$(BUILD)/kernel_diag.elf: $(BUILD)/entry.o $(BUILD)/switch32to64.o $(BUILD)/kernel.o $(BUILD)/divdi3.o $(BUILD)/ai_engine.o $(BUILD)/skill.o $(BUILD)/gguf.o $(BUILD)/net.o $(BUILD)/gui_diag.o $(BUILD)/font_vec_stub.o $(BUILD)/winloader.o $(BUILD)/win32.o $(BUILD)/linux_compat.o $(BUILD)/gdt.o $(BUILD)/syscall.o $(BUILD)/proc.o $(BUILD)/vfs.o $(BUILD)/perm.o $(BUILD)/clr.o $(BUILD)/mforms.o | $(BUILD)
	$(LD) $(LDFLAGS) -o $@ $^

$(BUILD)/kernel_diag.bin: $(BUILD)/kernel_diag.elf | $(BUILD)
	$(OBJCOPY) -O binary $< $@

# Rebuild the embedded blob with the DIAG kernel, link, then restore the
# normal kernel blob + object so `make uefi` stays intact.
$(BUILD)/bootx64_diag.so: $(BUILD)/bootuefi_diag.o $(BUILD)/enter_kernel_uefi.o $(BUILD)/get_embedded_uefi.o $(BUILD)/kernel_diag.bin | $(BUILD)
	cp -f $(BUILD)/kernel_diag.bin $(BUILD)/kernel.blob
	$(LD) -r -b binary -o $(BUILD)/kernel_blob.o $(BUILD)/kernel.blob
	$(LD) $(EFI_LDFLAGS) $(BUILD)/bootuefi_diag.o $(BUILD)/enter_kernel_uefi.o $(BUILD)/get_embedded_uefi.o $(BUILD)/kernel_blob.o -o $@ -lefi -lgnuefi
	cp -f $(BUILD)/kernel.bin $(BUILD)/kernel.blob
	$(LD) -r -b binary -o $(BUILD)/kernel_blob.o $(BUILD)/kernel.blob

$(BUILD)/BOOTX64_DIAG.EFI: $(BUILD)/bootx64_diag.so tools/gen_reloc.py | $(BUILD)
	$(OBJCOPY) -j .text -j .sdata -j .data -j .dynamic -j .dynsym -j .dynstr --target=efi-app-x86_64 $< $@
	$(PYTHON) tools/gen_reloc.py $(BUILD)/bootx64_diag.so $@

$(BUILD)/esp_diag.img: $(BUILD)/BOOTX64_DIAG.EFI $(BUILD)/kernel_diag.bin $(SFS_IMG) | $(BUILD)
	$(call ensure-pkg, mformat, $(PKG_MTOOLS))
	dd if=/dev/zero of=$@ bs=1M count=16 2>/dev/null
	$(MFORMAT) -i $@ ::
	$(MMD)     -i $@ ::/EFI
	$(MMD)     -i $@ ::/EFI/BOOT
	$(MCOPY)   -i $@ $(BUILD)/BOOTX64_DIAG.EFI ::/EFI/BOOT/BOOTX64.EFI
	$(MCOPY)   -i $@ $(BUILD)/kernel_diag.bin   ::/kernel.bin

$(UEFI_DIAG_IMG): $(BUILD)/esp_diag.img $(BUILD)/sfs_uefi.img $(BUILD)/kernel64.bin tools/make_gpt_uefi.py | $(BUILD)
	$(PYTHON) tools/make_gpt_uefi.py $(BUILD)/esp_diag.img $@ $(BUILD)/kernel64.bin $(BUILD)/sfs_uefi.img

uefi-diag: $(UEFI_DIAG_IMG)
	@echo "==> UEFI diagnostic image: $(UEFI_DIAG_IMG)"

# =====================================================================
#  Boot-logo load verification (kernel-level early logo)
#  Independent chain: a kernel compiled with -DBOOT_LOGO_TEST draws a
#  NexOS boot logo in kmain as the FIRST graphics-mode action (before the
#  GUI compositor).  Lets you triage real hardware: logo visible => kernel
#  loaded + framebuffer writable; no logo => kernel never reached graphics.
#  Does NOT touch the normal kernel.o / gui.o / kernel.bin / kernel.blob.
#  Output: build/os_uefi_logo.img (auto-boots as BOOTX64.EFI on real hw).
#  Use:   make boot-logo
# =====================================================================
UEFI_LOGO_IMG := $(BUILD)/os_uefi_logo.img

$(BUILD)/kernel_logo.o: kernel.cpp | $(BUILD)
	$(CC) $(CXXFLAGS) -DBOOT_LOGO_TEST -c kernel.cpp -o $@

$(BUILD)/kernel_logo.elf: $(BUILD)/entry.o $(BUILD)/switch32to64.o $(BUILD)/kernel_logo.o $(BUILD)/divdi3.o $(BUILD)/ai_engine.o $(BUILD)/gguf.o $(BUILD)/net.o $(BUILD)/gui.o $(BUILD)/winloader.o $(BUILD)/win32.o $(BUILD)/linux_compat.o $(BUILD)/gdt.o $(BUILD)/syscall.o $(BUILD)/proc.o $(BUILD)/vfs.o $(BUILD)/perm.o $(BUILD)/clr.o $(BUILD)/mforms.o | $(BUILD)
	$(LD) $(LDFLAGS) -o $@ $^

$(BUILD)/kernel_logo.bin: $(BUILD)/kernel_logo.elf | $(BUILD)
	$(OBJCOPY) -O binary $< $@

# Embed the logo kernel into the blob, link, then restore the normal kernel
# blob + object so `make uefi` stays intact.
$(BUILD)/bootx64_logo.so: $(BUILD)/bootuefi.o $(BUILD)/enter_kernel_uefi.o $(BUILD)/get_embedded_uefi.o $(BUILD)/kernel_logo.bin | $(BUILD)
	cp -f $(BUILD)/kernel_logo.bin $(BUILD)/kernel.blob
	$(LD) -r -b binary -o $(BUILD)/kernel_blob.o $(BUILD)/kernel.blob
	$(LD) $(EFI_LDFLAGS) $(BUILD)/bootuefi.o $(BUILD)/enter_kernel_uefi.o $(BUILD)/get_embedded_uefi.o $(BUILD)/kernel_blob.o -o $@ -lefi -lgnuefi
	cp -f $(BUILD)/kernel.bin $(BUILD)/kernel.blob
	$(LD) -r -b binary -o $(BUILD)/kernel_blob.o $(BUILD)/kernel.blob

$(BUILD)/BOOTX64_LOGO.EFI: $(BUILD)/bootx64_logo.so tools/gen_reloc.py | $(BUILD)
	$(OBJCOPY) -j .text -j .sdata -j .data -j .dynamic -j .dynsym -j .dynstr --target=efi-app-x86_64 $< $@
	$(PYTHON) tools/gen_reloc.py $(BUILD)/bootx64_logo.so $@

$(BUILD)/esp_logo.img: $(BUILD)/BOOTX64_LOGO.EFI $(BUILD)/kernel_logo.bin $(SFS_IMG) | $(BUILD)
	$(call ensure-pkg, mformat, $(PKG_MTOOLS))
	dd if=/dev/zero of=$@ bs=1M count=16 2>/dev/null
	$(MFORMAT) -i $@ ::
	$(MMD)     -i $@ ::/EFI
	$(MMD)     -i $@ ::/EFI/BOOT
	$(MCOPY)   -i $@ $(BUILD)/BOOTX64_LOGO.EFI ::/EFI/BOOT/BOOTX64.EFI
	$(MCOPY)   -i $@ $(BUILD)/kernel_logo.bin   ::/kernel.bin

$(UEFI_LOGO_IMG): $(BUILD)/esp_logo.img $(BUILD)/sfs_uefi.img $(BUILD)/kernel64.bin tools/make_gpt_uefi.py | $(BUILD)
	$(PYTHON) tools/make_gpt_uefi.py $(BUILD)/esp_logo.img $@ $(BUILD)/kernel64.bin $(BUILD)/sfs_uefi.img

boot-logo: $(UEFI_LOGO_IMG)
	@echo "==> UEFI boot-logo verification image: $(UEFI_LOGO_IMG)

# =====================================================================
#  Text-boot BIOS image (auto-GUI OFF) for headless security tests
#  Builds kernel.cpp with -DTEXT_BOOT so g_auto_gui defaults to 0: the OS
#  boots into the textual shell (text login, keyboard goes to the shell),
#  while VBE stays active so `winapp <file.exe>` still enters the GUI on
#  demand.  This is what test_perm_failsafe.py and test_win32_robust.py
#  need -- they type commands at the shell and screendump the framebuffer,
#  neither of which works once the OS boots straight into the desktop.
#  Mirrors the kernel_logo.o pattern.  Does NOT touch kernel.o / kernel.bin
#  / os.img, so the default auto-GUI image is unchanged.
#  Output: build/os_textboot.img   Use:  make textboot
# =====================================================================
TEXTBOOT_IMG := $(BUILD)/os_textboot.img

$(BUILD)/kernel_textboot.o: kernel.cpp | $(BUILD)
	$(CC) $(CXXFLAGS) -DTEXT_BOOT -c kernel.cpp -o $@

$(BUILD)/kernel_textboot.elf: $(BUILD)/entry.o $(BUILD)/switch32to64.o $(BUILD)/kernel_textboot.o $(BUILD)/divdi3.o $(BUILD)/ai_engine.o $(BUILD)/ai_plugin.o $(BUILD)/kb.o $(BUILD)/skill.o $(BUILD)/gguf.o $(BUILD)/net.o $(BUILD)/distnet.o $(BUILD)/gui.o $(BUILD)/font_vec_stub.o $(BUILD)/addrman.o $(BUILD)/winloader.o $(BUILD)/win32.o $(BUILD)/linux_compat.o $(BUILD)/gdt.o $(BUILD)/syscall.o $(BUILD)/proc.o $(BUILD)/vfs.o $(BUILD)/perm.o $(BUILD)/clr.o $(BUILD)/mforms.o | $(BUILD)
	$(LD) $(LDFLAGS) -o $@ $^

$(BUILD)/kernel_textboot.bin: $(BUILD)/kernel_textboot.elf | $(BUILD)
	$(OBJCOPY) -O binary $< $@
	@sz=$$(stat -c%s $@); \
	 if [ $$sz -gt 622592 ]; then \
	   echo "*** ERROR: kernel_textboot.bin is $$sz bytes (> 608 KiB / 0x98000)."; \
	   echo "***        stage2.asm loads only KERNEL_SECTORS=1216 sectors."; \
	   rm -f $@; exit 1; \
	 elif [ $$sz -gt 560332 ]; then \
	   echo "    [warn] kernel_textboot.bin $$sz bytes: >90% of the 608 KiB load window"; \
	 fi

# Same BIOS disk layout as os.img, but with the text-boot kernel.
$(TEXTBOOT_IMG): $(BUILD)/boot.bin $(BUILD)/stage2.bin $(BUILD)/kernel_textboot.bin $(BUILD)/kernel64.bin $(SFS_IMG) $(LINUX_SFS_IMG) | $(BUILD)
	cat $(BUILD)/boot.bin $(BUILD)/stage2.bin $(BUILD)/kernel_textboot.bin > $@
	truncate -s 4M $@
	dd if=$(BUILD)/kernel64.bin of=$@ bs=512 seek=2048 conv=notrunc 2>/dev/null
	dd if=$(SFS_IMG) of=$@ bs=512 seek=$(SFS_LBA) conv=notrunc 2>/dev/null
	dd if=$(LINUX_SFS_IMG) of=$@ bs=512 seek=$(LINUX_SFS_LBA) conv=notrunc 2>/dev/null
	@echo "==> Linux FS written at LBA $(LINUX_SFS_LBA)"
	@# NOTE: deliberately NO model embedding here.  The text-boot image is a
	@# headless shell-test vehicle; embedding the 941 MB test_model.gguf
	@# bloats it to ~1 GB and blows the C: drive when test scripts copy it.
	@echo "==> Text-boot BIOS Image built: $(TEXTBOOT_IMG) ($$(stat -c%s $@) bytes)"

textboot: $(TEXTBOOT_IMG)
	@echo "==> Text-boot image ready: $(TEXTBOOT_IMG)"

# =====================================================================
#  ISO build (BIOS no-emulation + UEFI for CD/DVD and USB)
#  - BIOS:  El Torito no-emulation (boot_cd.asm reads kernel from CD)
#  - UEFI:  El Torito no-emulation (esp.img with BOOTX64.EFI)
#  - GPT:   ESP appended as partition 2 for UEFI USB boot
# =====================================================================

# ----- CD boot sector (no-emulation, 2048-byte CD sectors) -----
$(BUILD)/boot_cd.bin: boot_cd.asm | $(BUILD)
	$(AS) $(ASFLAGS_BIN) boot_cd.asm -o $@

# ----- Texture-free SFS for the CD/ISO boot path -----
# The BIOS CD boot has no ATA disk to hold SFS, so boot_cd.asm streams this
# image off the CD into high RAM.  Textures (tex_*) are only used by the
# native fallback drawer, never by the managed C# shell, so they are excluded
# to keep the embedded image small.
$(BUILD)/sfs_cd.img: $(wildcard $(SFS_DIR)/*) tools/sfs_gen.py tools/tex_pack.py $(SFS_DIR)/shell.mex | $(BUILD)
	$(PYTHON) tools/tex_pack.py
	$(PYTHON) tools/sfs_gen.py $(SFS_DIR) $@ --exclude-tex

# ----- CD boot image (boot_cd.bin + kernel + texture-free SFS, 2048-byte aligned) -----
$(BUILD)/cd_boot.img: $(BUILD)/boot_cd.bin $(BUILD)/kernel.bin $(BUILD)/sfs_cd.img tools/make_cd_boot.py | $(BUILD)
	$(PYTHON) tools/make_cd_boot.py $(BUILD)/boot_cd.bin $(BUILD)/kernel.bin $@ $(BUILD)/sfs_cd.img

# ----- Prepare ISO root directory -----
# Includes /EFI/BOOT/BOOTX64.EFI in ISO9660 filesystem as a fallback for
# VirtualBox UEFI (Bug #19910: ISO9660 driver interferes with El Torito boot).
# Also includes kernel binaries for direct UEFI loading.
$(ISO_ROOT)/boot/cd_boot.img: $(BUILD)/cd_boot.img | $(ISO_ROOT)
	mkdir -p $(ISO_ROOT)/boot
	cp $< $@

$(ISO_ROOT)/boot/esp.img: $(BUILD)/esp.img | $(ISO_ROOT)
	mkdir -p $(ISO_ROOT)/boot
	cp $< $@

$(ISO_ROOT)/EFI/BOOT/BOOTX64.EFI: $(BUILD)/BOOTX64.EFI | $(ISO_ROOT)
	mkdir -p $(ISO_ROOT)/EFI/BOOT
	cp $< $@

$(ISO_ROOT)/kernel.bin: $(BUILD)/kernel.bin | $(ISO_ROOT)
	cp $< $@

$(ISO_ROOT):
	mkdir -p $(ISO_ROOT)/boot

# ----- Build hybrid ISO (BIOS + UEFI, VirtualBox-compatible) -----
# UEFI boot path 1: El Torito no-emulation (esp.img as FAT16 boot image)
# UEFI boot path 2: ISO9660 fallback (/EFI/BOOT/BOOTX64.EFI in filesystem)
# boot-load-size = 32768 (16MB ESP / 512 bytes per sector)
$(ISO): $(ISO_ROOT)/boot/cd_boot.img $(ISO_ROOT)/boot/esp.img \
        $(ISO_ROOT)/EFI/BOOT/BOOTX64.EFI \
        $(ISO_ROOT)/kernel.bin | $(BUILD)
	$(call ensure-pkg, xorriso, $(PKG_XORRISO))
	$(XORRISO) -as mkisofs \
	  -R -J -V MINIOS \
	  -c boot/boot.cat \
	  -b boot/cd_boot.img \
	  -no-emul-boot \
	  -boot-load-size 4 \
	  -boot-info-table \
	  -eltorito-alt-boot \
	  -e boot/esp.img \
	  -no-emul-boot \
	  -boot-load-size 32768 \
	  -append_partition 2 C12A7328-F81F-11D2-BA4B-00A0C93EC93B $(BUILD)/esp.img \
	  -output $(ISO) \
	  $(ISO_ROOT)
	@echo "==> ISO built: $(ISO) ($$(stat -c%s $(ISO)) bytes)"
	@echo "    BIOS:  no-emulation CD boot (boot_cd.asm + kernel)"
	@echo "    UEFI:  El Torito (esp.img) + ISO9660 fallback (/EFI/BOOT/BOOTX64.EFI)"
	@echo "    Burn to CD/DVD or dd to USB for real hardware"

iso: $(ISO)

# =====================================================================
#  Hybrid USB image (BIOS + UEFI bootable, 512-byte sectors)
#  Layout:
#    LBA 0-32:    boot.asm + stage2 (BIOS boot)
#    LBA 33+:     kernel.bin (32-bit kernel, BIOS path)
#    LBA 800:     SFS file system
#    LBA 1024+:   ESP partition (UEFI boot, FAT32)
#  MBR partition entry 1: ESP at LBA 1024, type 0xEF
# =====================================================================
$(HYBRID_IMG): $(IMG) $(BUILD)/esp.img tools/patch_mbr.py | $(BUILD)
	cp $(IMG) $@
	# Extend image to fit ESP at LBA 1024 (16MB ESP)
	truncate -s $$(( (1024 + 32768 + 64) * 512 )) $@
	# Write ESP at LBA 1024
	dd if=$(BUILD)/esp.img of=$@ bs=512 seek=1024 conv=notrunc 2>/dev/null
	# Patch MBR: partition 1 = ESP at LBA 1024, type 0xEF (EFI)
	$(PYTHON) -c "\
import struct,sys; \
d=bytearray(open('$@','rb').read()); \
p=bytearray(16); \
p[0]=0x80; p[1:4]=b'\x00\x21\x00'; p[4]=0xEF; p[5:8]=b'\xfe\xff\xff'; \
struct.pack_into('<I',p,8,1024); struct.pack_into('<I',p,12,32768); \
d[446:462]=p; open('$@','wb').write(d)"
	@echo "==> Hybrid USB image built: $(HYBRID_IMG) ($$(stat -c%s $@) bytes)"
	@echo "    BIOS:  boot.asm -> stage2 -> kernel (512-byte sectors)"
	@echo "    UEFI:  ESP at LBA 1024 (type 0xEF, BOOTX64.EFI)"
	@echo "    dd to USB: dd if=$(HYBRID_IMG) of=/dev/sdX bs=4M"

hybrid: $(HYBRID_IMG)

# =====================================================================
#  QEMU run / test targets
# =====================================================================

$(BUILD):
	mkdir -p $(BUILD)

# ----- Build 64-bit kernel + rebuild disk image with it -----
switch64: $(IMG)
	@echo "==> Disk image with 64-bit kernel ready: $(IMG)"
	@echo "    Run 'make run' and type 'switch' in the shell to switch to 64-bit"

# =====================================================================
#  Windowed-QEMU display backend
# =====================================================================
# WSLg publishes BOTH a Wayland socket ($WAYLAND_DISPLAY=wayland-0) and an
# X11 socket ($DISPLAY=:0). GTK auto-selects Wayland, but WSLg's compositor
# routinely tears the connection down mid-handshake and QEMU dies with:
#     Gdk-Message: Error 71 (Protocol error) dispatching to Wayland display.
# XWayland on the very same box is stable, so pin GTK/SDL to X11 whenever we
# detect that we are inside WSL. On a native Linux desktop /mnt/wslg does not
# exist, GUIENV stays empty, and GTK keeps whatever backend the user prefers.
GUIENV := $(shell [ -d /mnt/wslg ] && echo GDK_BACKEND=x11 SDL_VIDEODRIVER=x11)

# Display backend: the Windows build of QEMU 11.x ships a GTK frontend that
# throws  "gtk_window_set_mnemonics_visible: assertion 'GTK_IS_WINDOW (window)'
# failed"  and then renders a black/blank window.  SDL is the stable backend on
# native Windows, so default to it unless we are inside WSLg (where GTK+X11 is
# the intended path).  Override on the command line if you really want GTK:
#   make play DISPLAY_BACKEND=gtk
DISPLAY_BACKEND := $(shell [ -d /mnt/wslg ] && echo gtk || echo sdl)

# ----- UEFI: run in a QEMU window (default target) -----
run: $(UEFI_IMG)
	$(call ensure-pkg, qemu-system-x86_64, $(PKG_QEMU))
	$(if $(filter undefined,$(origin OVMF_CODE)),,$(if $(OVMF_CODE),,$(error OVMF_CODE not resolved -- install ovmf: sudo apt-get install $(PKG_OVMF))))
	cp $(OVMF_VARS) $(BUILD)/ovmf_vars.fd
	$(GUIENV) $(QEMU) -drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
		-drive if=pflash,format=raw,file=$(BUILD)/ovmf_vars.fd \
		-drive format=raw,file=$(UEFI_IMG) -m 64M -display $(DISPLAY_BACKEND)

# ----- BIOS: run in a QEMU window (legacy) -----
# NOTE: -m 4096 is used so the 64-bit kernel can map GB-scale model weights
# (PMM manages the low 4 GiB). The ring-3 user region lives at 64-128 MiB
# (USER_BASE 0x04000000 .. USER_END 0x08000000); with -m 64M that range has
# no backing RAM and the `user` command faults.
bios-run: $(IMG)
	$(call ensure-pkg, qemu-system-x86_64, $(PKG_QEMU))
	$(GUIENV) $(QEMU) -drive format=raw,file=$(IMG) -m 4096 \
		-display $(DISPLAY_BACKEND) \
		-net nic,model=ne2k_isa -net user,hostfwd=tcp::8080-:8080

# =====================================================================
#  Hands-on / manual testing entry points
# =====================================================================

# Interactive session in a real QEMU window (WSLg). Log in as root/admin,
# then try:  user | vfs /system/passwd | perm | perm reset | run hello32.exe
play: $(IMG)
	@echo "=== NexOS interactive session ==="
	@echo "login: root / admin"
	@echo "try:   user | vfs /system/passwd | perm | perm reset | help"
	@echo "quit:  close the window, or Ctrl-A X in the terminal"
	$(GUIENV) $(QEMU) -drive format=raw,file=$(IMG) -m 4096 \
		-vga std -display $(DISPLAY_BACKEND) -serial mon:stdio -no-reboot

# Fallback for the rare box where even XWayland misbehaves: SDL instead of GTK.
#   make play-sdl
play-sdl: $(ISO)
	@echo "=== NexOS interactive session (SDL backend) ==="
	@echo "login: root / admin   |   then type 'gui' for the Win11 desktop"
	$(GUIENV) $(QEMU) -cdrom $(ISO) -boot d -m 4096 \
		-vga std -display sdl -serial mon:stdio -no-reboot

# Last resort when no local display works at all (headless host, SSH, broken
# WSLg). Serves the screen over VNC on 127.0.0.1:5900 -- connect with any VNC
# client (Windows: TightVNC / RealVNC / mstsc-alternatives).
#   make play-vnc
play-vnc: $(ISO)
	@echo "=== NexOS over VNC -- connect a VNC client to 127.0.0.1:5900 ==="
	@echo "login: root / admin   |   then type 'gui' for the Win11 desktop"
	$(QEMU) -cdrom $(ISO) -boot d -m 4096 \
		-vga std -display vnc=127.0.0.1:0 -serial mon:stdio -no-reboot

# Inject local files (.exe/.zip/.bat/.com/...) and drive them headlessly.
#   make inject FILES="/tmp/a.exe /tmp/b.zip"
inject: $(IMG)
	python3 tools/vm_inject_test.py $(FILES)

# ----- Security / Foundation 0 test suite -----
# test-f0 boots the normal os.img with -vga none (g_vbe_active=false => text
# shell), so it needs no special variant.  test-fail / test-w32 need the
# text-boot image (auto-GUI OFF) so the shell is reachable and the VBE
# framebuffer can still be screendumped / entered on demand.
test-f0:   $(IMG) ; python3 tools/test_foundation0.py
test-fail: $(TEXTBOOT_IMG) ; python3 tools/test_perm_failsafe.py
test-w32:  $(TEXTBOOT_IMG) ; python3 tools/test_win32_robust.py
test-clr:   $(IMG) ; python3 tools/test_clr.py
test-rclick:$(IMG) ; python3 tools/test_rclick.py

# Run the whole security suite in sequence (sleeps let QEMU release its port).
# f0 uses the normal image; the two GUI-sensitive tests use the text-boot one.
test-sec: $(IMG) $(TEXTBOOT_IMG)
	@python3 tools/test_foundation0.py    && sleep 2 \
	&& python3 tools/test_perm_failsafe.py $(TEXTBOOT_IMG) && sleep 2 \
	&& python3 tools/test_win32_robust.py $(TEXTBOOT_IMG) \
	&& echo "" && echo "=== ALL SECURITY TESTS PASSED ==="

# ----- GGUF inference: host-side numeric cross-check -----
# Builds the freestanding engine for the host (-DGGUF_HOST_TEST swaps the
# serial logger for a stderr hook) and diffs its logits against a pure-Python
# reference forward pass over a real, tiny qwen2 GGUF that exercises every
# quantisation kernel (F32/F16/BF16/Q4_0/Q4_1/Q5_0/Q8_0/Q4_K/Q5_K/Q6_K).
$(BUILD)/test_model.gguf $(BUILD)/test_ref.txt: tools/make_test_gguf.py
	python3 tools/make_test_gguf.py

$(BUILD)/test_gguf: gguf_infer.cpp gguf_infer.h gguf.cpp gguf.h tools/test_gguf_infer.cpp
	g++ -O2 -Wall -I. -DGGUF_HOST_TEST -c gguf_infer.cpp          -o $(BUILD)/h_infer.o
	g++ -O2 -Wall -I.                  -c gguf.cpp                -o $(BUILD)/h_gguf.o
	g++ -O2 -Wall -I. -DGGUF_HOST_TEST -c tools/test_gguf_infer.cpp -o $(BUILD)/h_main.o
	g++ $(BUILD)/h_infer.o $(BUILD)/h_gguf.o $(BUILD)/h_main.o -o $@

test-gguf: $(BUILD)/test_gguf $(BUILD)/test_model.gguf $(BUILD)/test_ref.txt
	./$(BUILD)/test_gguf $(BUILD)/test_model.gguf $(BUILD)/test_ref.txt

# ----- UEFI: automated headless test (default) -----
test: $(UEFI_IMG)
	./test_uefi.sh $(UEFI_IMG)

# ----- BIOS: automated headless test (legacy) -----
bios-test: $(IMG)
	./test.sh $(IMG)

# ----- Disk write: raw ATA round-trip + SFS file round-trip (headless) -----
# Writes go to a throwaway copy (build/disk_rw.img); os.img itself is untouched.
test-diskrw: $(IMG)
	$(PYTHON) tools/test_disk_rw.py $(IMG)

# ----- AI engine: automated headless test -----
test-ai: $(IMG)
	./test_ai.sh $(IMG)

# ----- Network: automated HTTP server test -----
test-net: $(IMG)
	./test_net.sh $(IMG)

# ----- UEFI: run in a QEMU window with OVMF -----
uefi-run: $(UEFI_IMG)
	cp $(OVMF_VARS) $(BUILD)/ovmf_vars.fd
	$(QEMU) -drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
		-drive if=pflash,format=raw,file=$(BUILD)/ovmf_vars.fd \
		-drive format=raw,file=$(UEFI_IMG) -m 64M

# ----- UEFI: automated headless test -----
uefi-test: $(UEFI_IMG)
	./test_uefi.sh $(UEFI_IMG)

# ----- ISO: run in QEMU (BIOS mode, x86_64 for switch support) -----
iso-run: $(ISO)
	$(QEMU) -cdrom $(ISO) -boot d -m 2G

# ----- User data disk (secondary ATA VHD). Files persist across reboots. -----
data-vhd: build/data.vhd
build/data.vhd: tools/make_data_vhd.py
	python3 tools/make_data_vhd.py 8

# ----- ISO + data VHD: run with a persistent user disk (recommended) -----
iso-run-data: $(ISO) build/data.vhd
	$(QEMU) -cdrom $(ISO) -boot d -m 2G \
		-drive file=build/data.vhd,format=raw,if=ide,index=1
# ----- ISO: run in QEMU (UEFI mode) -----
iso-run-uefi: $(ISO)
	cp $(OVMF_VARS) $(BUILD)/ovmf_vars.fd
	$(QEMU) -drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
		-drive if=pflash,format=raw,file=$(BUILD)/ovmf_vars.fd \
		-cdrom $(ISO) -m 64M

# ----- ISO: automated headless test (BIOS) -----
iso-test: $(ISO)
	./test_iso.sh $(ISO)

# ----- ISO: automated headless test (UEFI) -----
iso-test-uefi: $(ISO)
	./test_uefi.sh $(ISO) iso

# ----- Hybrid USB: run in QEMU (BIOS mode, x86_64 for switch support) -----
hybrid-run: $(HYBRID_IMG)
	$(QEMU) -drive format=raw,file=$(HYBRID_IMG) -m 64M \
		-net nic,model=ne2k_isa -net user,hostfwd=tcp::8080-:8080

# ----- Hybrid USB: run in QEMU (UEFI mode) -----
hybrid-run-uefi: $(HYBRID_IMG)
	cp $(OVMF_VARS) $(BUILD)/ovmf_vars.fd
	$(QEMU) -drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
		-drive if=pflash,format=raw,file=$(BUILD)/ovmf_vars.fd \
		-drive format=raw,file=$(HYBRID_IMG) -m 64M

# ----- Hybrid USB: automated headless test (BIOS) -----
hybrid-test: $(HYBRID_IMG)
	./test.sh $(HYBRID_IMG)

# ----- Hybrid USB: automated headless test (UEFI) -----
hybrid-test-uefi: $(HYBRID_IMG)
	./test_uefi.sh $(HYBRID_IMG)

# ----- Run all tests -----
test-all: test test-ai test-net uefi-test iso-test iso-test-uefi hybrid-test hybrid-test-uefi
	@echo ""
	@echo "============================================"
	@echo "  ALL TESTS COMPLETE"
	@echo "  BIOS disk:      test"
	@echo "  AI engine:      test-ai"
	@echo "  Network/HTTP:   test-net"
	@echo "  UEFI disk:      uefi-test"
	@echo "  ISO BIOS:       iso-test"
	@echo "  ISO UEFI:       iso-test-uefi"
	@echo "  Hybrid BIOS:    hybrid-test"
	@echo "  Hybrid UEFI:    hybrid-test-uefi"
	@echo "============================================"

# =====================================================================
#  Misc
# =====================================================================

# ----- Inspect kernel symbols / disassembly -----
disasm: $(BUILD)/kernel.elf
	@echo "== entry point =="; nm $(BUILD)/kernel.elf | grep -E ' _start| kmain'
	@echo "== disassembly (head) =="; objdump -d -M intel $(BUILD)/kernel.elf | head -40

clean:
	rm -rf $(BUILD)

# =====================================================================
#  64-bit (long-mode) kernel  -  kernel64.cpp + shared gui/mforms/clr
#  Entered from the 32-bit kernel via the `switch` command, which
#  loads this binary from LBA 2048 and jumps to its entry (0x100000).
# =====================================================================
$(BUILD)/entry64.o: .attic64/entry64.asm | $(BUILD)
	$(AS) $(AS64FLAGS) -o $@ .attic64/entry64.asm

$(BUILD)/switch64to32.o: .attic64/switch64to32.asm | $(BUILD)
	$(AS) $(AS64FLAGS) -o $@ .attic64/switch64to32.asm

$(BUILD)/switch32to64.o: .attic64/switch32to64.asm | $(BUILD)
	$(AS) $(ASFLAGS_ELF) -o $@ .attic64/switch32to64.asm

$(BUILD)/kernel64.o: .attic64/kernel64.cpp | $(BUILD)
	$(CC64) $(CXX64FLAGS) -c .attic64/kernel64.cpp -o $@

$(BUILD)/gui64.o: gui.cpp logo.h font_vec.h | $(BUILD)
	$(CC64) $(CXX64FLAGS) -c gui.cpp -o $@

$(BUILD)/ai_engine64.o: ai_engine.cpp | $(BUILD)
	$(CC64) $(CXX64FLAGS) -c ai_engine.cpp -o $@

$(BUILD)/kb64.o: kb.cpp kb.h | $(BUILD)
	$(CC64) $(CXX64FLAGS) -c kb.cpp -o $@

# Plugin catalogue / manager (Settings > Plugins UI backend + `plugin` cmd).
$(BUILD)/ai_plugin64.o: ai_plugin.cpp ai_plugin.h | $(BUILD)
	$(CC64) $(CXX64FLAGS) -c ai_plugin.cpp -o $@

$(BUILD)/gguf64.o: gguf.cpp gguf.h | $(BUILD)
	$(CC64) $(CXX64FLAGS) -c gguf.cpp -o $@

# Real GGUF inference is 64-bit only: GB-scale weights need the 4 GiB PMM pool
# and the 1 GiB-page identity map that long mode provides.
$(BUILD)/gguf_infer64.o: gguf_infer.cpp gguf_infer.h gguf.h | $(BUILD)
	$(CC64) $(CXX64FLAGS) -c gguf_infer.cpp -o $@

# Phase 0 ggml adapter layer (Route A): memory / file / gguf-loader backends.
# Freestanding 64-bit C, compiled with the same CC64 driver as the rest of
# the long-mode kernel.
$(BUILD)/memory_adapter64.o: memory_adapter.c memory_adapter.h | $(BUILD)
	$(CC64) $(CXX64FLAGS) -c memory_adapter.c -o $@

$(BUILD)/file_adapter64.o: file_adapter.c file_adapter.h | $(BUILD)
	$(CC64) $(CXX64FLAGS) -c file_adapter.c -o $@

$(BUILD)/gguf_loader64.o: gguf_loader.c gguf_loader.h file_adapter.h | $(BUILD)
	$(CC64) $(CXX64FLAGS) -c gguf_loader.c -o $@

# In-kernel verified-fact knowledge base (truth-from-practice + authoritative
# fallback). Feeds kb_build_prompt() into the chat path (cmd_ask / demo harness).
$(BUILD)/knowledge_base64.o: knowledge_base.c knowledge_base.h | $(BUILD)
	$(CC64) $(CXX64FLAGS) -c knowledge_base.c -o $@

$(BUILD)/net64.o: net.cpp | $(BUILD)
	$(CC64) $(CXX64FLAGS) -c net.cpp -o $@

# Distributed compute fabric (discovery + task dispatch + AI inference task).
$(BUILD)/distnet64.o: distnet.cpp distnet.h | $(BUILD)
	$(CC64) $(CXX64FLAGS) -c distnet.cpp -o $@

$(BUILD)/winloader64.o: winloader.cpp | $(BUILD)
	$(CC64) $(CXX64FLAGS) -c winloader.cpp -o $@

$(BUILD)/win32_64.o: win32.cpp | $(BUILD)
	$(CC64) $(CXX64FLAGS) -c win32.cpp -o $@

$(BUILD)/gdt64.o: gdt.cpp | $(BUILD)
	$(CC64) $(CXX64FLAGS) -c gdt.cpp -o $@

$(BUILD)/proc64.o: proc.cpp | $(BUILD)
	$(CC64) $(CXX64FLAGS) -c proc.cpp -o $@

$(BUILD)/vfs64.o: vfs.cpp | $(BUILD)
	$(CC64) $(CXX64FLAGS) -c vfs.cpp -o $@

$(BUILD)/perm64.o: perm.cpp | $(BUILD)
	$(CC64) $(CXX64FLAGS) -c perm.cpp -o $@

$(BUILD)/clr64.o: clr.cpp | $(BUILD)
	$(CC64) $(CXX64FLAGS) -c clr.cpp -o $@

$(BUILD)/mforms64.o: mforms.cpp | $(BUILD)
	$(CC64) $(CXX64FLAGS) -fno-optimize-sibling-calls -c mforms.cpp -o $@

# ----- Link 64-bit kernel ELF (entry64.o first => _start64 at 0x100000) -----
$(BUILD)/kernel64.elf: $(BUILD)/entry64.o $(BUILD)/switch64to32.o $(BUILD)/kernel64.o $(BUILD)/ai_engine64.o $(BUILD)/ai_plugin64.o $(BUILD)/kb64.o $(BUILD)/gguf64.o $(BUILD)/gguf_infer64.o $(BUILD)/memory_adapter64.o $(BUILD)/file_adapter64.o $(BUILD)/gguf_loader64.o $(BUILD)/knowledge_base64.o $(BUILD)/net64.o $(BUILD)/distnet64.o $(BUILD)/gui64.o $(BUILD)/font_vec64.o $(BUILD)/addrman64.o $(BUILD)/winloader64.o $(BUILD)/win32_64.o $(BUILD)/gdt64.o $(BUILD)/proc64.o $(BUILD)/vfs64.o $(BUILD)/perm64.o $(BUILD)/clr64.o $(BUILD)/mforms64.o $(BUILD)/smp_bringup.o $(BUILD)/ap_trampoline.o .attic64/linker64.ld | $(BUILD)
	$(LD64) $(LDFLAGS64) -o $@ $(BUILD)/entry64.o $(BUILD)/switch64to32.o $(BUILD)/kernel64.o $(BUILD)/ai_engine64.o $(BUILD)/ai_plugin64.o $(BUILD)/kb64.o $(BUILD)/gguf64.o $(BUILD)/gguf_infer64.o $(BUILD)/memory_adapter64.o $(BUILD)/file_adapter64.o $(BUILD)/gguf_loader64.o $(BUILD)/knowledge_base64.o $(BUILD)/net64.o $(BUILD)/distnet64.o $(BUILD)/gui64.o $(BUILD)/font_vec64.o $(BUILD)/addrman64.o $(BUILD)/winloader64.o $(BUILD)/win32_64.o $(BUILD)/gdt64.o $(BUILD)/proc64.o $(BUILD)/vfs64.o $(BUILD)/perm64.o $(BUILD)/clr64.o $(BUILD)/mforms64.o $(BUILD)/smp_bringup.o $(BUILD)/ap_trampoline.o

$(BUILD)/smp_bringup.o: .attic64/smp_bringup.cpp .attic64/smp64.h | $(BUILD)
	$(CC64) $(CXX64FLAGS) -c .attic64/smp_bringup.cpp -o $@

# ELF relocations are needed, then wrap it in an ELF64 object via objcopy
# so it can be linked.  This exposes _binary_ap_trampoline_bin_start/end.
# ----- Extract flat 64-bit kernel binary -----
# A 32-bit (i686-elf) objcopy cannot read the ELF64 input, so the 64-bit
# binutils objcopy must be used here (OBJCOPY64).  On multilib Linux builds
# OBJCOPY64 defaults to $(OBJCOPY) and everything is one toolchain.
OBJCOPY64 ?= $(OBJCOPY)
$(BUILD)/kernel64.bin: $(BUILD)/kernel64.elf | $(BUILD)
	$(OBJCOPY64) -O binary $< $@
