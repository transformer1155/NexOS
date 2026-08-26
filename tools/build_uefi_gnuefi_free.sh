#!/usr/bin/env bash
# =====================================================================
#  build_uefi_gnuefi_free.sh
#  Build a faithful BOOTX64.EFI that embeds the CURRENT build/kernel.bin,
#  WITHOUT the gnu-efi package.  Uses the x86_64-elf cross toolchain and
#  the repo's tools/gen_reloc.py (PE32+ .reloc synthesis).
#
#  Output: build/BOOTX64.EFI  (overwrites the stale one)
#
#  NOTE: the x86_64-elf-* tools are native Windows .exe, so all paths
#  passed to them MUST be in Windows style (D:/...).  Shell builtins
#  (mkdir/cp) keep using the Unix-style $BUILD/$PROJ.
# =====================================================================
set -e

PROJ="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$PROJ/build"
# Windows-style variants for the native toolchain / python
PROJ_W="$(cygpath -w "$PROJ")"
BUILD_W="$(cygpath -w "$BUILD")"

TOOL="C:/Users/trans/elf_tools/bin"
CC="$TOOL/x86_64-elf-gcc"
LD="$TOOL/x86_64-elf-ld"
OBJCOPY="$TOOL/x86_64-elf-objcopy"
READELF="$TOOL/x86_64-elf-readelf"
PYTHON="C:/Users/trans/.workbuddy/binaries/python/versions/3.13.12/python.exe"

mkdir -p "$BUILD"

echo "==> [1] embedded kernel blob (current kernel.bin)"
cp "$BUILD/kernel.bin" "$BUILD/kernel.blob"
"$LD" -r -b binary -o "$BUILD_W/kernel_blob.o" "$BUILD_W/kernel.blob"

# ld -b binary derives the symbol name from the FULL path it receives
# (native Windows ld gets D:/.../build/kernel.blob -> _binary_D__...).
# Rename to the canonical _binary_build_kernel_blob_* that get_embedded.S
# expects, so the link resolves deterministically regardless of path.
OLD_PREFIX="_binary_$(printf '%s' "$BUILD_W" | sed 's/[^A-Za-z0-9]/_/g')_kernel_blob"
"$OBJCOPY" --redefine-sym "${OLD_PREFIX}_start=_binary_build_kernel_blob_start" \
           --redefine-sym "${OLD_PREFIX}_end=_binary_build_kernel_blob_end" \
           "$BUILD_W/kernel_blob.o"
echo "    blob symbols -> _binary_build_kernel_blob_{start,end}"

echo "==> [1b] embedded 64-bit kernel blob (current kernel64.bin)"
cp "$BUILD/kernel64.bin" "$BUILD/kernel64.blob"
"$LD" -r -b binary -o "$BUILD_W/kernel64_blob.o" "$BUILD_W/kernel64.blob"
OLD_PREFIX64="_binary_$(printf '%s' "$BUILD_W" | sed 's/[^A-Za-z0-9]/_/g')_kernel64_blob"
"$OBJCOPY" --redefine-sym "${OLD_PREFIX64}_start=_binary_build_kernel64_blob_start" \
           --redefine-sym "${OLD_PREFIX64}_end=_binary_build_kernel64_blob_end" \
           "$BUILD_W/kernel64_blob.o"
echo "    blob symbols -> _binary_build_kernel64_blob_{start,end}"

echo "==> [1c] embedded SFS image blob (current sfs.img)"
# Why: under UEFI (q35/AHCI) the kernels' legacy IDE PIO ata_read_sector reads
# all zeros, so Sfs::init() can never find the "SFS\0" superblock -> shell.mex
# is "file not found" -> the managed Win11 shell draws nothing -> black screen.
# We therefore embed the whole SFS image in the EFI binary and stage it into
# RAM before ExitBootServices, handing the address off at 0x0900 (the same
# RAM-SFS handoff the 32-bit CD-boot path already uses).
if [ ! -f "$BUILD/sfs.img" ]; then
    echo "    *** FATAL: $BUILD/sfs.img missing - build it first (make sfs) ***"
    exit 1
fi
cp "$BUILD/sfs.img" "$BUILD/sfs.blob"
"$LD" -r -b binary -o "$BUILD_W/sfs_blob.o" "$BUILD_W/sfs.blob"
OLD_PREFIX_SFS="_binary_$(printf '%s' "$BUILD_W" | sed 's/[^A-Za-z0-9]/_/g')_sfs_blob"
"$OBJCOPY" --redefine-sym "${OLD_PREFIX_SFS}_start=_binary_build_sfs_blob_start" \
           --redefine-sym "${OLD_PREFIX_SFS}_end=_binary_build_sfs_blob_end" \
           "$BUILD_W/sfs_blob.o"
echo "    blob symbols -> _binary_build_sfs_blob_{start,end} ($(stat -c%s "$BUILD/sfs.img") bytes)"

echo "==> [2] compile loader objects (-mabi=ms, gnu-efi-free)"
CFLAGS="-mabi=ms -fshort-wchar -ffreestanding -fno-stack-protector"
CFLAGS="$CFLAGS -mno-red-zone -fvisibility=hidden -fPIC -fno-plt"
CFLAGS="$CFLAGS -I$PROJ_W/uefi/gf_inc -I$PROJ_W/uefi -Wall"

"$CC" $CFLAGS $EXTRA_CFLAGS -c "$PROJ_W/uefi/bootuefi.c"    -o "$BUILD_W/bootuefi_gf.o"
"$CC" $CFLAGS -c "$PROJ_W/uefi/efi_min.c"     -o "$BUILD_W/efi_min.o"
"$CC" $CFLAGS -c "$PROJ_W/uefi/crt0_min.S"    -o "$BUILD_W/crt0_min.o"
"$CC" $CFLAGS -c "$PROJ_W/uefi/enter_kernel.S" -o "$BUILD_W/enter_kernel_gf.o"
"$CC" $CFLAGS -c "$PROJ_W/uefi/get_embedded.S" -o "$BUILD_W/get_embedded_gf.o"

echo "==> [3] link shared object (PE32+ reloc model)"
"$LD" -shared -Bsymbolic -znocombreloc -e _start -T "$PROJ_W/uefi/efi_min.lds" \
    -o "$BUILD_W/bootx64_gf.so" \
    "$BUILD_W/crt0_min.o" \
    "$BUILD_W/bootuefi_gf.o" \
    "$BUILD_W/efi_min.o" \
    "$BUILD_W/enter_kernel_gf.o" \
    "$BUILD_W/get_embedded_gf.o" \
    "$BUILD_W/kernel_blob.o" \
    "$BUILD_W/kernel64_blob.o" \
    "$BUILD_W/sfs_blob.o"

echo "==> [4] relocation sanity check (only R_X86_64_RELATIVE expected)"
"$READELF" -r "$BUILD_W/bootx64_gf.so" > "$BUILD/reloc_gf.txt" 2>&1 || true
{ grep -E 'R_X86_64_' "$BUILD/reloc_gf.txt" | awk '{print $3}' | sort | uniq -c ; } || true
echo "    (full dump: build/reloc_gf.txt)"

echo "==> [5] ELF -> PE32+ EFI application (pure-Python, no PE-capable objcopy)"
"$PYTHON" "$PROJ_W/tools/elf2efi.py" "$BUILD_W/bootx64_gf.so" "$BUILD_W/BOOTX64.EFI"

echo "==> [done] $(cygpath -w "$BUILD/BOOTX64.EFI") ($(stat -c%s "$BUILD/BOOTX64.EFI") bytes)"
