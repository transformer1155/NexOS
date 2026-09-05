#!/usr/bin/env bash
# =====================================================================
#  test_iso.sh - headless test for ISO BIOS boot (CD boot path)
#  Boots from CD in BIOS mode, waits for kernel, dumps VGA, runs shell
# =====================================================================
set -euo pipefail
cd "$(dirname "$0")"

ISO="${1:-build/os.iso}"
OUT="build/iso_bios_test.log"
D1="build/iso_vga1.bin"
D2="build/iso_vga2.bin"
rm -f "$D1" "$D2" "$OUT"

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
      '_')   echo "sendkey shift-minus" ;;
      '-')   echo "sendkey minus" ;;
      [A-Z]) echo "sendkey shift-${ch,,}" ;;
      *)     echo "sendkey $ch" ;;
    esac
    sleep 0.06
  done
  echo "sendkey ret"; sleep "$delay"
}

echo "==> ISO BIOS boot test: booting from CD..."
(
  sleep 4                              # wait for kernel to load from CD
  echo "memsave 0xb8000 0x1000 $D1"; sleep 0.5

  # Run basic shell commands
  type_line "help"
  type_line "echo hello_from_iso"
  echo "memsave 0xb8000 0x1000 $D2"; sleep 0.5

  echo "quit"
) | qemu-system-x86_64 -cdrom "$ISO" -boot d -m 64M -display none -no-reboot \
     -monitor stdio > "$OUT" 2>&1 || true

decode(){ python3 - "$1" <<'PY'
import sys
d=open(sys.argv[1],'rb').read()
c=bytes(d[i] for i in range(0,len(d),2)).decode('latin-1')
print("\n".join(c[i:i+80].rstrip('\x00') for i in range(0,len(c),80) if c[i:i+80].strip()))
PY
}

echo; echo "===== VGA dump 1: boot screen ====="; [ -s "$D1" ] && decode "$D1" || echo "(no dump)"
echo; echo "===== VGA dump 2: after shell commands ====="; [ -s "$D2" ] && decode "$D2" || echo "(no dump)"
echo

echo "===== Assertions ====="
python3 - "$D1" "$D2" <<'PY'
import sys, os
def load(p):
    if not os.path.exists(p) or os.path.getsize(p) == 0:
        return ""
    d=open(p,'rb').read()
    c=bytes(d[i] for i in range(0,len(d),2)).decode('latin-1')
    return c
d1=load(sys.argv[1]); d2=load(sys.argv[2])
ok=True
def chk(c,m):
    global ok; print(("PASS" if c else "FAIL")+": "+m)
    if not c: ok=False

chk('Hello world' in d1 or 'MiniOS' in d1, "kernel boots from ISO (banner visible)")
chk('DISK ERR' not in d1,                "no disk read error")
chk('hello_from_iso' in d2,              "shell executes echo command from ISO boot")
chk('shutdown' in d2 or 'Tab' in d2,     "help command works from ISO boot")

if ok:
    print("\n=== ISO BIOS BOOT TEST PASSED ===")
else:
    print("\n=== ISO BIOS BOOT TEST FAILED ===")
sys.exit(0 if ok else 1)
PY
