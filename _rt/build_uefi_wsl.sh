#!/usr/bin/env bash
# gnu-efi-free BOOTX64.EFI build, adapted to run under WSL with the system
# gcc/ld (the original tools/build_uefi_gnuefi_free.sh needs MSYS cygpath and
# the native Windows x86_64-elf toolchain).  Reuses the Makefile's blob objects
# (build/kernel_blob.o etc.), whose _binary_build_*_blob_{start,end} symbols are
# exactly what uefi/get_embedded.S and bootuefi.c expect.
set -e
PROJ="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$PROJ/build"
cd "$PROJ"

CC=gcc
LD=ld
PY=python3

# --- blobs (relative path == _binary_build_<name>_blob_start) ---------------
[ -f "$BUILD/kernel.bin" ]   || { echo "missing build/kernel.bin";   exit 1; }
[ -f "$BUILD/kernel64.bin" ] || { echo "missing build/kernel64.bin"; exit 1; }
[ -f "$BUILD/sfs.img" ]      || { echo "missing build/sfs.img (make sfs)"; exit 1; }
cp build/kernel.bin   build/kernel.blob
cp build/kernel64.bin build/kernel64.blob
cp build/sfs.img      build/sfs.blob
$LD -r -b binary -o build/gf_kernel_blob.o   build/kernel.blob
$LD -r -b binary -o build/gf_kernel64_blob.o build/kernel64.blob
$LD -r -b binary -o build/gf_sfs_blob.o      build/sfs.blob

# --- compile loader objects (-mabi=ms, gnu-efi-free shim) -------------------
CFLAGS="-m64 -mabi=ms -fshort-wchar -ffreestanding -fno-stack-protector"
CFLAGS="$CFLAGS -mno-red-zone -fvisibility=hidden -fPIC -fno-plt"
CFLAGS="$CFLAGS -I$PROJ/uefi/gf_inc -I$PROJ/uefi -Wall -Wno-unused-variable"
$CC $CFLAGS -c uefi/bootuefi.c    -o "$BUILD/bootuefi_gf.o"
$CC $CFLAGS -c uefi/efi_min.c     -o "$BUILD/efi_min.o"
$CC $CFLAGS -c uefi/crt0_min.S    -o "$BUILD/crt0_min.o"
$CC $CFLAGS -c uefi/enter_kernel.S -o "$BUILD/enter_kernel_gf.o"
$CC $CFLAGS -c uefi/get_embedded.S -o "$BUILD/get_embedded_gf.o"

# --- link shared object (PE32+ reloc model) --------------------------------
$LD -shared -Bsymbolic -znocombreloc -e _start -T "$PROJ/uefi/efi_min.lds" \
    -o "$BUILD/bootx64_gf.so" \
    "$BUILD/crt0_min.o" \
    "$BUILD/bootuefi_gf.o" \
    "$BUILD/efi_min.o" \
    "$BUILD/enter_kernel_gf.o" \
    "$BUILD/get_embedded_gf.o" \
    "$BUILD/gf_kernel_blob.o" \
    "$BUILD/gf_kernel64_blob.o" \
    "$BUILD/gf_sfs_blob.o"

# --- ELF -> PE32+ EFI application ------------------------------------------
$PY tools/elf2efi.py "$BUILD/bootx64_gf.so" "$BUILD/BOOTX64.EFI"
echo "[done] $BUILD/BOOTX64.EFI ($(stat -c%s "$BUILD/BOOTX64.EFI") bytes)"
