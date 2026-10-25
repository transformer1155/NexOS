#!/usr/bin/env bash
# ============================================================================
# fastboot.sh - fast headless boot verifier for NexOS
# ----------------------------------------------------------------------------
# WHY: a full verify cycle used to be `make` (31 s) + QEMU to GUI (41 s) = ~73 s,
#      repeated dozens of times per debugging session.
#
# WHAT: boots the image headless and exits the moment a SENTINEL string appears
#      on the serial console, instead of always waiting for the GUI.  The driver
#      scan and most boot diagnostics complete long before GUI mode, so this
#      turns a 41 s wait into ~1 s.
#
# USAGE:
#   bash tools/fastboot.sh                     # default sentinel: scan complete
#   bash tools/fastboot.sh 'GUI. Entered'      # wait for a later point
#   bash tools/fastboot.sh 'kmain64' 60        # sentinel + timeout seconds
#   bash tools/fastboot.sh '' 20               # no sentinel: full 20 s capture
#
# Output: prints the captured serial log to stdout; exit 0 if the sentinel was
# seen (or if no sentinel was requested and the run completed).
# ============================================================================
set -u
cd "$(dirname "$0")/.." || exit 1

SENTINEL="${1-scan complete}"
TIMEOUT="${2:-45}"
IMG="${IMG:-build/os_v2.img}"
LOG="${LOG:-/mnt/e/NexOS/_fastboot.log}"

if [ ! -f "$IMG" ]; then echo "fastboot: missing $IMG" >&2; exit 2; fi

rm -f "$LOG"
qemu-system-i386 -m 512 -drive file="$IMG",format=raw,if=ide \
  -display none -serial file:"$LOG" -no-reboot &
QP=$!

# Poll for the sentinel at 50 ms resolution.  QEMU writes the file directly,
# so this needs no monitor socket and works on a shared Windows mount.
SEEN=0
i=0
MAX=$(( TIMEOUT * 20 ))
while [ $i -lt $MAX ]; do
  if [ -n "$SENTINEL" ] && grep -qa "$SENTINEL" "$LOG" 2>/dev/null; then SEEN=1; break; fi
  kill -0 $QP 2>/dev/null || break
  i=$((i+1))
  sleep 0.05
done

kill $QP 2>/dev/null; wait $QP 2>/dev/null

# Serial carries 0x00 padding in places; strip it so the log is readable.
tr -d '\000' < "$LOG" 2>/dev/null

if [ -n "$SENTINEL" ] && [ $SEEN -eq 0 ]; then
  echo "fastboot: sentinel not seen: $SENTINEL" >&2
  exit 1
fi
exit 0