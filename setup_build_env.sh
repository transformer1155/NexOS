#!/usr/bin/env bash
# =====================================================================
#  setup_build_env.sh  -  MyOS one-shot build-environment installer
#
#  Target: WSL / native Linux (Debian/Ubuntu based).
#  Run as a normal user (it will sudo internally when needed).
#
#  Installs everything the Makefile / tools need:
#    - core C/asm toolchain    (nasm, g++, binutils, gnu-efi)
#    - Python + build-time deps (Pillow, pypinyin)
#    - optional groups (QEMU/OVMF, mtools/xorriso for ISO,
#                       mingw-w64 for WinPE, dotnet-sdk-8.0 for csharp)
#
#  Usage:
#    ./setup_build_env.sh                 # core + python + run/iso deps
#    ./setup_build_env.sh --all           # also mingw + dotnet
#    ./setup_build_env.sh --with-mingw --with-dotnet
#    ./setup_build_env.sh --no-qemu       # skip QEMU/OVMF
# =====================================================================
set -euo pipefail

# ---- parse args -----------------------------------------------------
WITH_MINGW=0
WITH_DOTNET=0
WITH_QEMU=1

for arg in "$@"; do
  case "$arg" in
    --all)            WITH_MINGW=1; WITH_DOTNET=1 ;;
    --with-mingw)     WITH_MINGW=1 ;;
    --with-dotnet)    WITH_DOTNET=1 ;;
    --no-qemu)        WITH_QEMU=0 ;;
    -h|--help)
      grep '^#' "$0" | sed 's/^# \{0,1\}//'
      exit 0 ;;
    *) echo "unknown arg: $arg (try --help)" >&2; exit 1 ;;
  esac
done

# ---- detect sudo ----------------------------------------------------
if [[ $EUID -eq 0 ]]; then
  SUDO=""
else
  SUDO="sudo"
fi

echo "==> MyOS build-env setup ($(uname -a))"
echo "    mingw=${WITH_MINGW} dotnet=${WITH_DOTNET} qemu=${WITH_QEMU}"

# ---- apt packages ---------------------------------------------------
APT_CORE=( nasm g++ binutils gnu-efi python3 python3-pip python3-venv )
APT_ISO=( mtools xorriso )
APT_QEMU=( qemu-system-x86 ovmf )
APT_MINGW=( gcc-mingw-w64-x86-64 gcc-mingw-w64-i686 )
APT_DOTNET=( dotnet-sdk-8.0 )

pkgs=( "${APT_CORE[@]}" "${APT_ISO[@]}" )
(( WITH_QEMU ))   && pkgs+=( "${APT_QEMU[@]}" )
(( WITH_MINGW ))  && pkgs+=( "${APT_MINGW[@]}" )
(( WITH_DOTNET )) && pkgs+=( "${APT_DOTNET[@]}" )

echo "==> apt-get update"
$SUDO apt-get update -qq

echo "==> installing apt packages: ${pkgs[*]}"
$SUDO apt-get install -y -qq "${pkgs[@]}"

# ---- Python build-time deps ----------------------------------------
# Pillow: used by most tools/*.py (gen_*, ai_*, *_check, test_* ...)
# pypinyin: used by tools/gen_ime_dict.py
echo "==> installing Python packages (Pillow, pypinyin)"
$SUDO python3 -m pip install --break-system-packages -q --upgrade pip || true
$SUDO python3 -m pip install --break-system-packages -q Pillow pypinyin

# ---- verify ---------------------------------------------------------
echo "==> verifying toolchain"
for b in nasm g++ ld objcopy python3; do
  command -v "$b" >/dev/null 2>&1 || { echo "!! missing: $b"; exit 1; }
done
python3 -c "import PIL, pypinyin; print('  PIL', PIL.__version__)" \
  || echo "!! python deps check failed"

echo "==> core build environment ready."
(( WITH_QEMU ))   && echo "   [run]   QEMU + OVMF installed  -> make run / make uefi"
(( WITH_MINGW ))  && echo "   [winpe] mingw-w64 installed    -> make winpe"
(( WITH_DOTNET )) && echo "   [csharp] dotnet-sdk-8.0 installed -> make csharp"

echo "   next: WSL=1 make        (core build)"
echo "         WSL=1 make run    (boot in QEMU)"
