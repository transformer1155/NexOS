#!/usr/bin/env bash
# =====================================================================
#  test_net.sh  -  Network stack integration test
# ---------------------------------------------------------------------
#  Tests: NE2000 NIC init, netinfo command, HTTP server (if network
#  forwarding is available)
# =====================================================================
set -euo pipefail
cd "$(dirname "$0")"

SRC="${1:-build/os.img}"
IMG="build/test_net.img"
VGA="build/net_vga.bin"
SERIAL="build/net_serial.log"

rm -f "$VGA" "$SERIAL" "$IMG"
cp "$SRC" "$IMG"

echo "==> Network Stack Integration Test"
echo "    Kernel: $(stat -c%s build/kernel.bin) bytes"

# ---- Phase 1: Boot with NE2000 NIC and check netinfo ----
echo ""
echo "===== Phase 1: Boot + netinfo command ====="

(
  sleep 4
  # Type "netinfo"
  for c in n e t i n f o; do echo "sendkey $c"; sleep 0.05; done
  echo "sendkey ret"; sleep 1
  echo "memsave 0xb8000 0x1000 $VGA"; sleep 0.5
  echo "quit"
) | qemu-system-x86_64 -drive format=raw,file="$IMG" -m 64M \
    -display none -no-reboot \
    -serial file:"$SERIAL" -monitor stdio \
    -net nic,model=ne2k_isa -net user > /dev/null 2>&1 || true

# Decode VGA text buffer
VGA_TEXT=""
if [ -s "$VGA" ]; then
    VGA_TEXT=$(python3 - "$VGA" <<'PY'
import sys
d = open(sys.argv[1], 'rb').read()
c = bytes(d[i] for i in range(0, len(d), 2)).decode('latin-1')
lines = [c[i:i+80].rstrip('\x00') for i in range(0, len(c), 80)]
print("\n".join(l for l in lines if l.strip()))
PY
)
fi
echo "$VGA_TEXT"

# Check serial log for network init
SERIAL_TEXT=""
if [ -s "$SERIAL" ]; then
    SERIAL_TEXT=$(grep -E "\[NET\]|\[K8\]" "$SERIAL" 2>/dev/null || echo "")
fi
echo ""
echo "Serial (network lines):"
echo "$SERIAL_TEXT"

# ---- Assertions ----
echo ""
echo "===== Assertions ====="
python3 - "$VGA" "$SERIAL" <<'PY'
import sys, os

def load_vga(p):
    if not os.path.exists(p) or os.path.getsize(p) == 0:
        return ""
    d = open(p, 'rb').read()
    c = bytes(d[i] for i in range(0, len(d), 2)).decode('latin-1')
    return c

def load_serial(p):
    if not os.path.exists(p) or os.path.getsize(p) == 0:
        return ""
    return open(p, 'r', errors='replace').read()

vga = load_vga(sys.argv[1])
ser = load_serial(sys.argv[2])
ok = True

def chk(c, m):
    global ok
    print(("PASS" if c else "FAIL") + ": " + m)
    if not c: ok = False

# Boot screen shows network status
chk('NET:' in vga and 'UP' in vga, "Boot screen shows NET: UP status")
chk('10.0.2.15' in vga, "Boot screen shows IP address")

# netinfo command output
chk('Network Status' in vga, "netinfo command shows Network Status")
chk('NIC: UP' in vga, "netinfo shows NIC: UP")
chk('10.0.2.15' in vga, "netinfo shows IP 10.0.2.15")
chk('Port: 8080' in vga, "netinfo shows HTTP port 8080")
chk('MAC:' in vga, "netinfo shows MAC address")

# Serial log confirms NE2000 init
chk('NE2000 initialized' in ser, "Serial log: NE2000 initialized")
chk('Network ready' in ser, "Serial log: Network ready")
chk('K8' in ser and 'success' in ser.lower(), "Serial log: K8 network init success")

if ok:
    print("\n=== ALL NETWORK TESTS PASSED ===")
else:
    print("\n=== SOME NETWORK TESTS FAILED ===")
sys.exit(0 if ok else 1)
PY

# ---- Phase 2: HTTP server test (optional, requires host forwarding) ----
echo ""
echo "===== Phase 2: HTTP server test (optional) ====="
echo "    (Requires network egress - may be blocked in sandbox)"
echo "    To test manually: make run, then open http://localhost:8080"
