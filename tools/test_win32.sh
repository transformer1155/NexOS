#!/usr/bin/env bash
# =====================================================================
#  tools/test_win32.sh - headless QEMU test for the Win32 subsystem
# ---------------------------------------------------------------------
#  Session 1 (text mode):  lsfs / winver / winenv / reg query / winapp /i
#  Session 2 (graphics)  : winapp hello32.exe -> GUI window -> screendump
# =====================================================================
set -euo pipefail
cd "$(dirname "$0")/.."

SRC="${1:-build/os.img}"
IMG="build/w32test.img"
OUT="build/qemu_win32.log"
T1="build/w32_txt1.bin"; T2="build/w32_txt2.bin"; T3="build/w32_txt3.bin"
PPM="build/w32_gui.ppm"

rm -f "$T1" "$T2" "$T3" "$OUT" "$IMG" "$PPM"
[ -f "$SRC" ] || { echo "missing $SRC - run 'make build/os.img' first"; exit 1; }
cp "$SRC" "$IMG"

# Type a string then Enter, using QEMU monitor sendkey names.
type_line(){
  local s="$1" delay="${2:-0.25}" ch
  for ((i=0;i<${#s};i++)); do
    ch="${s:$i:1}"
    case "$ch" in
      ' ')   echo "sendkey spc" ;;
      '.')   echo "sendkey dot" ;;
      '/')   echo "sendkey slash" ;;
      '\')   echo "sendkey backslash" ;;
      '-')   echo "sendkey minus" ;;
      '_')   echo "sendkey shift-minus" ;;
      [A-Z]) echo "sendkey shift-${ch,,}" ;;
      *)     echo "sendkey $ch" ;;
    esac
    sleep 0.05
  done
  echo "sendkey ret"; sleep "$delay"
}

QEMU_ARGS=(-drive format=raw,file="$IMG" -m 128M -vga std -display none -no-reboot -monitor stdio)

echo "==> Session 1: registry / environment / PE inspection (text mode)"
(
  sleep 3.0
  type_line "lsfs" 0.6
  type_line "winver" 0.8
  type_line "winenv" 0.8
  echo "memsave 0xb8000 0x1000 $T1"; sleep 0.5

  type_line "clear" 0.4
  type_line "reg query HKLM\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion" 1.2
  echo "memsave 0xb8000 0x1000 $T2"; sleep 0.5

  type_line "clear" 0.4
  type_line "winapp /i hello32.exe" 1.5
  echo "memsave 0xb8000 0x1000 $T3"; sleep 0.5
  echo "quit"
) | qemu-system-x86_64 "${QEMU_ARGS[@]}" > "$OUT" 2>&1 || true

echo "==> Session 2: winapp hello32.exe -> GUI window (framebuffer capture)"
(
  sleep 3.0
  type_line "winapp hello32.exe" 3.0
  sleep 2.5
  echo "screendump $PPM"; sleep 1.5
  echo "quit"
) | qemu-system-x86_64 "${QEMU_ARGS[@]}" >> "$OUT" 2>&1 || true

decode(){ python3 - "$1" <<'PY'
import sys
d=open(sys.argv[1],'rb').read()
c=bytes(d[i] for i in range(0,len(d),2)).decode('latin-1')
print("\n".join(c[i:i+80].rstrip('\x00').rstrip() for i in range(0,len(c),80) if c[i:i+80].strip()))
PY
}

echo; echo "===== Phase 1: lsfs / winver / winenv ====="
[ -s "$T1" ] && decode "$T1" || echo "(no dump)"
echo; echo "===== Phase 2: reg query CurrentVersion ====="
[ -s "$T2" ] && decode "$T2" || echo "(no dump)"
echo; echo "===== Phase 3: winapp /i hello32.exe ====="
[ -s "$T3" ] && decode "$T3" || echo "(no dump)"

echo; echo "===== Phase 4: GUI framebuffer ====="
if [ -s "$PPM" ]; then
  python3 - "$PPM" <<'PY'
import sys
p = sys.argv[1]
with open(p,'rb') as f: data=f.read()
# parse P6 header
def tok(d, i):
    while d[i:i+1].isspace(): i+=1
    if d[i:i+1]==b'#':
        while d[i:i+1] not in (b'\n', b''): i+=1
        return tok(d,i)
    j=i
    while not d[j:j+1].isspace(): j+=1
    return d[i:j], j
magic,i = tok(data,0); w,i = tok(data,i); h,i = tok(data,i); mx,i = tok(data,i)
i+=1
w=int(w); h=int(h)
px=data[i:]
print(f"screendump: {magic.decode()} {w}x{h}")
cols={}
for y in range(0,h,2):
    for x in range(0,w,2):
        o=(y*w+x)*3
        c=px[o:o+3]
        if len(c)==3: cols[c]=cols.get(c,0)+1
top=sorted(cols.items(), key=lambda kv:-kv[1])[:8]
tot=sum(cols.values())
print("distinct colors:", len(cols))
for c,n in top:
    print(f"  #{c[0]:02X}{c[1]:02X}{c[2]:02X}  {100.0*n/tot:5.1f}%")
PY
  # convert to PNG if possible for visual inspection
  if command -v convert >/dev/null 2>&1; then convert "$PPM" build/w32_gui.png && echo "PNG: build/w32_gui.png"
  elif command -v pnmtopng >/dev/null 2>&1; then pnmtopng "$PPM" > build/w32_gui.png && echo "PNG: build/w32_gui.png"
  else python3 - "$PPM" build/w32_gui.png <<'PY'
import sys, zlib, struct
src,dst=sys.argv[1],sys.argv[2]
d=open(src,'rb').read()
def tok(d,i):
    while d[i:i+1].isspace(): i+=1
    if d[i:i+1]==b'#':
        while d[i:i+1] not in (b'\n',b''): i+=1
        return tok(d,i)
    j=i
    while not d[j:j+1].isspace(): j+=1
    return d[i:j],j
m,i=tok(d,0); w,i=tok(d,i); h,i=tok(d,i); mx,i=tok(d,i); i+=1
w=int(w); h=int(h); px=d[i:]
raw=b''.join(b'\x00'+px[y*w*3:(y+1)*w*3] for y in range(h))
def chunk(t,data):
    c=t+data
    return struct.pack('>I',len(data))+c+struct.pack('>I',zlib.crc32(c)&0xffffffff)
png=b'\x89PNG\r\n\x1a\n'
png+=chunk(b'IHDR',struct.pack('>IIBBBBB',w,h,8,2,0,0,0))
png+=chunk(b'IDAT',zlib.compress(raw,6))
png+=chunk(b'IEND',b'')
open(dst,'wb').write(png)
print("PNG:",dst)
PY
  fi
else
  echo "(no screendump produced)"
fi
