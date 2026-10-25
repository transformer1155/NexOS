#!/usr/bin/env bash
# Headless smoke test using -nographic so VGA text (incl. 'gui' output) is
# captured on stdio.  Monitor also on stdio for sendkey input.
set -u
IMG="${1:-build/os.img}"
LOG="build/smoke_ser.log"
rm -f "$LOG"
k() { local ch="$1"; [ "$ch" = ' ' ] && echo "sendkey spc" || echo "sendkey $ch"; }
(
  sleep 5.0
  k g; sleep 0.2
  k u; sleep 0.2
  k i; sleep 0.2
  echo "sendkey ret"; sleep 4.0    # 'gui' -> Win11 Desktop
  echo "mouse_move 640 360"; sleep 0.3
  echo "sendkey ret"; sleep 2.0    # click on desktop
  sleep 1.0
  echo "quit"
) | qemu-system-x86_64 -drive format=raw,file="$IMG" -m 128M \
      -nographic -no-reboot -monitor stdio > "$LOG" 2>&1
echo "=== MFORMS / Entering Win11 markers ==="
grep -c "MFORMS" "$LOG" | sed 's/^/MFORMS lines: /'
grep -i "Entering Win11\|Win11 Desktop\|mforms" "$LOG" | head
echo "=== fault markers (expect none) ==="
grep -i "triple\|#PF\|GPF\|panic\|unhandled\|exception" "$LOG" | head || echo "none"
