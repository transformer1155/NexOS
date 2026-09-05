# =====================================================================
#  wsl_setup.mk  -  WSL / native-Linux toolchain resolution
#
#  This file is ONLY included when WSL=1 (see the top of the main
#  Makefile).  It does two things:
#    1. Resolves the system toolchain into absolute paths (AS, CC, LD,
#       OBJCOPY, PYTHON, MINGW*, QEMU, mtools, xorriso, OVMF, ...).
#    2. Provides $(call ensure-pkg, binary, apt-package) which lazily
#       installs a *missing* tool right before a target that needs it,
#       so a plain `make` never pulls qemu/dotnet/mingw/ovmf unless you
#       actually ask for `run` / `uefi` / `iso` / `winpe` / `csharp`.
#
#  No apt-get runs at parse time -- installs only happen inside recipe
#  bodies (where a TTY is available for the sudo password).
# =====================================================================

# ---- apt package names ----
PKG_NASM    := nasm
PKG_BINUTILS:= binutils
PKG_GCC     := g++
PKG_GNUEFI  := gnu-efi
PKG_OVMF    := ovmf
PKG_QEMU    := qemu-system-x86
PKG_MTOOLS  := mtools
PKG_XORRISO := xorriso
PKG_MINGW   := gcc-mingw-w64-x86-64 gcc-mingw-w64-i686
PKG_DOTNET  := dotnet-sdk-8.0

# ---- resolve a binary to its absolute path (no install at parse time) ----
# Overrides GNU make's built-in defaults (cc/as/...) but still lets an
# explicit command-line / environment override win.
# $(1)=VAR  $(2)=binary  $(3)=apt-package (for the warning)
define resolve-bin
ifeq ($$(origin $(1)),default)
  $(1) := $$(shell command -v $(2) 2>/dev/null)
  ifeq ($$($(1)),)
    $$(warning [wsl] '$(2)' not found -- add it with: \
      sudo apt-get install $(3))
  endif
  export $(1)
endif
endef

# ---- core, always-needed tools (resolve; warn if missing) ----
$(eval $(call resolve-bin, AS,       nasm,    $(PKG_NASM)))
$(eval $(call resolve-bin, CC,       g++,     $(PKG_GCC)))
$(eval $(call resolve-bin, LD,       ld,      $(PKG_BINUTILS)))
$(eval $(call resolve-bin, OBJCOPY,  objcopy, $(PKG_BINUTILS)))
$(eval $(call resolve-bin, PYTHON,   python3, python3))

# ---- gnu-efi (headers + crt0 + linker script) ----
EFI_INC  ?= $(shell ls -d /usr/include/efi 2>/dev/null)
EFI_LIB  ?= $(shell ls -d /usr/lib 2>/dev/null)
EFI_CRT0 ?= $(EFI_LIB)/crt0-efi-x86_64.o
EFI_LDS  ?= $(EFI_LIB)/elf_x86_64_efi.lds
ifeq ($(EFI_INC),)
  $(warning [wsl] gnu-efi headers not found -- UEFI build needs: \
    sudo apt-get install $(PKG_GNUEFI))
endif

# ---- OVMF firmware ----
OVMF_CODE ?= $(firstword $(wildcard /usr/share/OVMF/OVMF_CODE.fd /usr/share/edk2/ovmf/OVMF_CODE.fd))
OVMF_VARS ?= $(firstword $(wildcard /usr/share/OVMF/OVMF_VARS.fd /usr/share/edk2/ovmf/OVMF_VARS.fd))
ifeq ($(OVMF_CODE),)
  $(warning [wsl] OVMF firmware not found -- UEFI run needs: \
    sudo apt-get install $(PKG_OVMF))
endif

# ---- mingw-w64 (only needed for `make winpe`) ----
MINGW64 ?= x86_64-w64-mingw32-gcc
MINGW32 ?= i686-w64-mingw32-gcc

# ---- QEMU (native) ----
QEMU ?= $(shell command -v qemu-system-x86_64 2>/dev/null)

# ---- mtools / xorriso (only needed for `make uefi` / `make iso`) ----
MFORMAT ?= $(shell command -v mformat 2>/dev/null)
MCOPY   ?= $(shell command -v mcopy 2>/dev/null)
MMD     ?= $(shell command -v mmd 2>/dev/null)
XORRISO ?= $(shell command -v xorriso 2>/dev/null)

# dotnet is resolved lazily in the csharp targets.
DOTNET  ?= $(shell command -v dotnet 2>/dev/null)

# =====================================================================
#  $(call ensure-pkg, binary, apt-package)
#  Install a missing tool inside a recipe body (TTY available for sudo).
# =====================================================================
define ensure-pkg
@if ! command -v $(1) >/dev/null 2>&1; then \
  echo "[wsl] installing $(2) (needed for $@) ..."; \
  sudo apt-get update -qq && sudo apt-get install -y -qq $(2) || \
    { echo "!! failed to install $(2); run: sudo apt-get install $(2)"; exit 1; }; \
fi
endef
