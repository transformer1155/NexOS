#!/usr/bin/env bash
# =====================================================================
#  build_bootx64_efi.sh
#  Build BOOTX64.EFI with the gnu-efi-FREE path: uefi/gf_inc headers +
#  uefi/efi_min.c + uefi/crt0_min.S + tools/elf2efi.py, using the host
#  gcc/ld.  No gnu-efi package and no native x86_64-elf toolchain needed,
#  so it runs on Linux/WSL as well as in an MSYS shell.
#
#  Why not the gnu-efi variant: that PE loaded under OVMF but faulted with
#  #UD at the entry, while this fully-PIC (0 base relocations) image boots.
#
#  Three blobs are linked in (relative input paths -> the symbols
#  _binary_build_<name>_blob_{start,end} that uefi/get_embedded.S and
#  bootuefi.c expect):
#    build/kernel.bin    -> the 32-bit kernel, copied to 0x10000
#    build/kernel64.bin  -> the 64-bit kernel, RAM-staged for `switch64`
#    build/sfs.img       -> the whole SFS, RAM-staged (AHCI PIO cannot read
#                           the disk, so Sfs::init() would never mount it)
# =====================================================================
set -e
PROJ="$(cd "$(dirname "$0")/.." && pwd)"
cd "$PROJ"

CC="${CC:-gcc}"
LD="${LD:-ld}"
PY="${PYTHON:-python3}"

[ -f build/kernel.bin ]   || { echo "!! missing build/kernel.bin";          exit 1; }
[ -f build/kernel64.bin ] || { echo "!! missing build/kernel64.bin";        exit 1; }
[ -f build/sfs.img ]      || { echo "!! missing build/sfs.img (make sfs)";  exit 1; }

echo "==> [1/4] embedded blobs (kernel / kernel64 / sfs)"
cp build/kernel.bin   build/kernel.blob
cp build/kernel64.bin build/kernel64.blob
cp build/sfs.img      build/sfs.blob
$LD -r -b binary -o build/efi_kernel_blob.o   build/kernel.blob
$LD -r -b binary -o build/efi_kernel64_blob.o build/kernel64.blob
$LD -r -b binary -o build/efi_sfs_blob.o      build/sfs.blob

echo "==> [2/4] compile loader objects (-mabi=ms, gnu-efi-free shim)"
CFLAGS="-m64 -mabi=ms -fshort-wchar -ffreestanding -fno-stack-protector"
CFLAGS="$CFLAGS -mno-red-zone -fvisibility=hidden -fPIC -fno-plt"
CFLAGS="$CFLAGS -I$PROJ/uefi/gf_inc -I$PROJ/uefi -Wall -Wno-unused-variable"
$CC $CFLAGS -c uefi/bootuefi.c     -o build/efi_bootuefi.o
$CC $CFLAGS -c uefi/efi_min.c      -o build/efi_min.o
$CC $CFLAGS -c uefi/crt0_min.S     -o build/efi_crt0.o
$CC $CFLAGS -c uefi/enter_kernel.S -o build/efi_enter_kernel.o
$CC $CFLAGS -c uefi/get_embedded.S -o build/efi_get_embedded.o

echo "==> [3/4] link shared object (PE32+ reloc model)"
$LD -shared -Bsymbolic -znocombreloc -e _start -T "$PROJ/uefi/efi_min.lds" \
    -o build/bootx64_gf.so \
    build/efi_crt0.o build/efi_bootuefi.o build/efi_min.o \
    build/efi_enter_kernel.o build/efi_get_embedded.o \
    build/efi_kernel_blob.o build/efi_kernel64_blob.o build/efi_sfs_blob.o

echo "==> [4/4] ELF -> PE32+ EFI application"
$PY tools/elf2efi.py build/bootx64_gf.so build/BOOTX64.EFI
echo "==> BOOTX64.EFI ready: $(stat -c%s build/BOOTX64.EFI) bytes"
