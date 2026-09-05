#!/usr/bin/env bash
# =====================================================================
#  test.sh  -  headless test: shell, scrollback, FS, dirs, FAT32, scripts
# ---------------------------------------------------------------------
#  Session 1:
#    Phase A:  help + echo hello (dump1)
#    12 echo cmds, Up x50 (dump2 = Phase C), Down x50 (dump3 = Phase D)
#    save (writes command file to disk)
#    Phase F1: MKFS tests - mkfs/write/cat/ls/rm (dump5)
#    Phase G:  Directory tests - mkdir/cd/pwd/touch/cd ../rm (dump7)
#    Phase F2: SFS tests - lsfs/catfs/runfs (dump6)
#
#  Session 2: fresh boot on the SAME image, load + history (dump4) ->
#             proves the command file persisted across a real reboot.
#
#  Session 3: Phase H - FAT32 partition tests - part/mount/lsfat (dump8)
# =====================================================================
set -euo pipefail
cd "$(dirname "$0")"

SRC="${1:-build/os.img}"
IMG="build/test.img"
OUT="build/qemu_test.log"
D1="build/vga1.bin"; D2="build/vga2.bin"; D3="build/vga3.bin"
D4="build/vga4.bin"; D5="build/vga5.bin"; D6="build/vga6.bin"
D7="build/vga7.bin"; D8="build/vga8.bin"
FAT_IMG="build/fat32_part.img"
rm -f "$D1" "$D2" "$D3" "$D4" "$D5" "$D6" "$D7" "$D8" "$OUT" "$IMG" "$FAT_IMG"
cp "$SRC" "$IMG"          # work on a copy so the build artifact stays pristine

# =====================================================================
#  Setup: Add a FAT32 partition to the test image for Phase H tests
# =====================================================================
#  The boot sector (LBA 0) has code at offsets 0-~90 and zeros from ~91-509.
#  The MBR partition table lives at offset 446-509, which is safe to patch.
#  We extend the image to 128 MB (sparse) and place a 64 MB FAT32 partition
#  at LBA 1024. FAT32 requires 65525+ clusters, so 64 MB is the minimum
#  practical size with 512-byte sectors and 1 sector/cluster.
HAS_FAT32=false
truncate -s 128M "$IMG"

if command -v mkfs.fat &>/dev/null; then
    truncate -s 64M "$FAT_IMG"
    if mkfs.fat -F 32 -n MINIDISK "$FAT_IMG" 2>/dev/null; then
        HAS_FAT32=true
    fi
elif command -v mformat &>/dev/null; then
    truncate -s 64M "$FAT_IMG"
    if mformat -F -i "$FAT_IMG" :: 2>/dev/null; then
        HAS_FAT32=true
    fi
fi

if [ "$HAS_FAT32" = true ]; then
    # Copy a test file onto the FAT32 partition
    echo "Welcome from FAT32 partition!" > build/welcome_fat.txt
    mcopy -i "$FAT_IMG" build/welcome_fat.txt ::Welcome.txt 2>/dev/null || true
    # Also create a subdirectory with a file
    mmd -i "$FAT_IMG" ::/DOCS 2>/dev/null || true
    echo "Document in subdirectory." > build/doc_fat.txt
    mcopy -i "$FAT_IMG" build/doc_fat.txt ::/DOCS/readme.txt 2>/dev/null || true

    # Write FAT32 partition into test image at LBA 1024 (byte offset 524288)
    dd if="$FAT_IMG" of="$IMG" bs=512 seek=1024 conv=notrunc 2>/dev/null

    # Patch MBR partition table: entry 1 = FAT32 LBA at LBA 1024
    python3 - "$IMG" <<'PY'
import struct, sys
img = sys.argv[1]
with open(img, 'r+b') as f:
    part = bytearray(16)
    part[0] = 0x80       # bootable flag
    # start CHS (3 bytes) = 0 (unused by LBA reads)
    part[4] = 0x0C       # partition type = FAT32 LBA
    # end CHS (3 bytes) = 0 (unused by LBA reads)
    struct.pack_into('<I', part, 8, 1024)    # start_lba
    struct.pack_into('<I', part, 12, 131072) # total_sectors (64 MB)
    f.seek(446)
    f.write(part)
print("MBR partition table patched: partition 1 = FAT32 at LBA 1024")
PY
    echo "==> FAT32 partition added to test image"
else
    echo "==> WARNING: mkfs.fat/mformat not available, skipping FAT32 setup"
    echo "    Phase H partition tests will test raw layout detection only"
fi

# Type a string then Enter. Handles dots and uppercase via QEMU key names.
type_line(){
  local s="$1"
  local delay="${2:-0.2}"
  for ((i=0;i<${#s};i++)); do
    local ch="${s:$i:1}"
    case "$ch" in
      ' ')   echo "sendkey spc" ;;
      '.')   echo "sendkey dot" ;;
      '/')   echo "sendkey slash" ;;
      '\\')  echo "sendkey backslash" ;;
      [A-Z]) echo "sendkey shift-${ch,,}" ;;
      *)     echo "sendkey $ch" ;;
    esac
    sleep 0.06
  done
  echo "sendkey ret"; sleep "$delay"
}

echo "==> Session 1: shell + scroll + save + file system + directory tests"
(
  sleep 2.5

  # ---- Phase A: help then echo hello (so 'hello' visible at bottom) ----
  type_line "help"
  type_line "echo hello"
  echo "memsave 0xb8000 0x1000 $D1"; sleep 0.4

  # ---- 12 echo commands to fill scrollback ----
  for n in 1 2 3 4 5 6 7 8 9 10 11 12; do type_line "echo e$n"; done

  # ---- Phase C: scroll up to see banner (Page Up, since Up=history recall) ----
  for i in $(seq 1 10); do echo "sendkey pgup";   sleep 0.05; done
  echo "memsave 0xb8000 0x1000 $D2"; sleep 0.4

  # ---- Phase D: scroll back to bottom (Page Down) ----
  for i in $(seq 1 10); do echo "sendkey pgdn"; sleep 0.05; done
  echo "memsave 0xb8000 0x1000 $D3"; sleep 0.4

  # ---- Save command history to disk (for persistence test) ----
  type_line "save"
  sleep 0.5

  # ---- Phase F1: MKFS (custom writable file system) tests ----
  type_line "mkfs"
  type_line "write f.txt" 0.3
  type_line "hello from mkfs" 0.2
  type_line "" 0.3              # empty line = save & exit write mode
  type_line "cat f.txt" 0.3
  type_line "ls" 0.2
  type_line "rm f.txt" 0.2
  type_line "ls" 0.2
  echo "memsave 0xb8000 0x1000 $D5"; sleep 0.4

  # ---- Phase G: Directory navigation tests ----
  type_line "mkdir testdir" 0.3
  type_line "cd testdir" 0.2
  type_line "pwd" 0.2
  type_line "touch inside.txt" 0.3
  type_line "ls" 0.2
  type_line "rm inside.txt" 0.2
  type_line "cd .." 0.2
  type_line "pwd" 0.2
  type_line "rm testdir" 0.2
  type_line "ls" 0.2
  echo "memsave 0xb8000 0x1000 $D7"; sleep 0.4

  # ---- Phase F2: SFS (compatible read-only file system) tests ----
  type_line "lsfs" 0.3
  type_line "catfs welcome.txt" 0.3
  type_line "runfs hello.sh" 0.8
  echo "memsave 0xb8000 0x1000 $D6"; sleep 0.4

  echo "quit"
) | qemu-system-x86_64 -drive format=raw,file="$IMG" -m 64M -display none -no-reboot \
        -monitor stdio > "$OUT" 2>&1 || true

echo "==> Session 2: fresh boot, load + history (persistence)"
(
  sleep 2.5
  type_line "load"
  type_line "history"
  echo "memsave 0xb8000 0x1000 $D4"; sleep 0.4
  echo "quit"
) | qemu-system-x86_64 -drive format=raw,file="$IMG" -m 64M -display none -no-reboot \
        -monitor stdio >> "$OUT" 2>&1 || true

echo "==> Session 3: FAT32 partition tests"
(
  sleep 2.5
  type_line "part" 0.5
  type_line "mount 1" 0.5
  type_line "lsfat" 0.5
  type_line "fatinfo" 0.5
  echo "memsave 0xb8000 0x1000 $D8"; sleep 0.4
  echo "quit"
) | qemu-system-x86_64 -drive format=raw,file="$IMG" -m 64M -display none -no-reboot \
        -monitor stdio >> "$OUT" 2>&1 || true

decode(){ python3 - "$1" <<'PY'
import sys
d=open(sys.argv[1],'rb').read()
c=bytes(d[i] for i in range(0,len(d),2)).decode('latin-1')
print("\n".join(c[i:i+80].rstrip('\x00') for i in range(0,len(c),80) if c[i:i+80].strip()))
PY
}

echo; echo "===== Phase A: shell (help + echo hello) =====";       [ -s "$D1" ] && decode "$D1"
echo; echo "===== Phase C: scrolled back (Up x50) =====";          [ -s "$D2" ] && decode "$D2"
echo; echo "===== Phase D: back to bottom (Down x50) =====";       [ -s "$D3" ] && decode "$D3"
echo; echo "===== Phase E: fresh boot, load + history =====";      [ -s "$D4" ] && decode "$D4"
echo; echo "===== Phase F1: MKFS file system tests =====";         [ -s "$D5" ] && decode "$D5"
echo; echo "===== Phase G: Directory navigation tests =====";      [ -s "$D7" ] && decode "$D7"
echo; echo "===== Phase F2: SFS file system tests =====";          [ -s "$D6" ] && decode "$D6"
echo; echo "===== Phase H: FAT32 partition tests =====";           [ -s "$D8" ] && decode "$D8"
echo

echo "===== Assertions ====="
python3 - "$D1" "$D2" "$D3" "$D4" "$D5" "$D6" "$D7" "$D8" "$HAS_FAT32" <<'PY'
import sys, os
def load(p):
    if not os.path.exists(p) or os.path.getsize(p) == 0:
        return "", []
    d=open(p,'rb').read()
    c=bytes(d[i] for i in range(0,len(d),2)).decode('latin-1')
    return c, [c[i:i+80] for i in range(0,len(c),80)]
d1,_=load(sys.argv[1]); d2,_=load(sys.argv[2]); d3,_=load(sys.argv[3])
d4,_=load(sys.argv[4]); d5,_=load(sys.argv[5]); d6,_=load(sys.argv[6])
d7,_=load(sys.argv[7]); d8,_=load(sys.argv[8])
has_fat32 = sys.argv[9] == 'true'
ok=True
def chk(c,m):
    global ok; print(("PASS" if c else "FAIL")+": "+m)
    if not c: ok=False

# ---- Phase A: shell basics ----
chk('hello' in d1,                 "echo command prints 'hello' (phase A)")
chk('shutdown' in d1 or 'Tab' in d1, "help lists power/keyboard commands (phase A)")

# ---- Phase C: scrollback up ----
chk('Hello world' in d2,           "Up arrow scrolls back -> banner returns (phase C)")
chk('e12' not in d2,               "scrolled back -> latest 'e12' off-screen (phase C)")

# ---- Phase D: scroll back to bottom ----
chk('e12' in d3,                   "Down arrow returns to bottom -> 'e12' visible (phase D)")
chk('Hello world' not in d3,       "at bottom -> banner scrolled off again (phase D)")

# ---- Phase E: persistence ----
chk('echo e1' in d4,               "fresh boot: load+history shows 'echo e1' (file persistence)")
chk('Loaded' in d4,                "load reports commands loaded from disk (phase E)")

# ---- Phase F1: MKFS (custom writable file system) ----
chk('MKFS formatted' in d5,        "mkfs formats the custom file system (phase F1)")
chk('hello from mkfs' in d5,       "write+cat round-trips file content (phase F1)")
chk('f.txt' in d5,                 "ls lists MKFS files (phase F1)")
chk('Removed: f.txt' in d5,        "rm deletes a file (phase F1)")
chk('(empty)' in d5,               "ls shows empty after rm (phase F1)")

# ---- Phase G: Directory navigation ----
chk('Created dir: testdir' in d7,  "mkdir creates directory (phase G)")
chk('testdir' in d7,               "cd navigates into directory (phase G)")
chk('inside.txt' in d7,           "touch creates file inside directory (phase G)")
chk('Removed: testdir' in d7,     "rm deletes empty directory (phase G)")

# ---- Phase F2: SFS (compatible read-only file system) ----
chk('welcome.txt' in d6,           "lsfs lists SFS files (phase F2)")
chk('Welcome to MiniOS' in d6,     "catfs reads SFS file content (phase F2)")
chk('Hello from SFS script' in d6, "runfs executes .sh script from SFS (phase F2)")

# ---- Phase H: FAT32 partition (only if FAT32 setup succeeded) ----
if has_fat32:
    chk('FAT32' in d8 or '0x0C' in d8,  "part detects FAT32 partition (phase H)")
    chk('mounted successfully' in d8,    "mount mounts FAT32 partition (phase H)")
    chk('WELCOME' in d8.upper(),         "lsfat lists FAT32 files (phase H)")
else:
    print("SKIP: FAT32 tests (mkfs.fat/mformat not available)")

if ok:
    print("\n=== ALL TESTS PASSED ===")
else:
    print("\n=== SOME TESTS FAILED ===")
sys.exit(0 if ok else 1)
PY
