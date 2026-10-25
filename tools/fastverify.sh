#!/usr/bin/env bash
# ============================================================================
# fastverify.sh - one-command rebuild + headless verify for NexOS
# ----------------------------------------------------------------------------
# WHY THIS EXISTS
#   A verify cycle used to be: `make` (33 s) + QEMU boot to GUI (41 s) = ~74 s.
#   Repeated dozens of times per session that is hours of waiting.  Two things
#   were wasteful:
#     1. QEMU was always run until the GUI appeared, even when the thing under
#        test (the driver scan) completes ~200 log lines earlier.
#     2. `make` rebuilds BOTH kernels and BOTH images for any shared-source
#        edit, which is inherent to the build but does not need the GUI wait.
#
#   With the early-exit boot below the verify half drops to ~1-3 s.
#
# USAGE
#   bash tools/fastverify.sh                 # rebuild, wait for driver scan
#   bash tools/fastverify.sh 'GUI. Entered'  # rebuild, wait for GUI instead
#   bash tools/fastverify.sh 'kmain64' 90    # wait for a 64-bit sentinel
#   SKIP_MAKE=1 bash tools/fastverify.sh     # verify the current image only
#   GREP='DRIVER|FBCON' bash tools/fastverify.sh   # filter the printed log
#
# Exit status: 0 if the build succeeded and the sentinel was seen.
# ============================================================================
set -u
cd "$(dirname "$0")/.." || exit 1

SENTINEL="${1-scan complete}"
TIMEOUT="${2:-45}"
LOG="${LOG:-/mnt/e/NexOS/_fastverify.log}"

if [ "${SKIP_MAKE:-0}" != "1" ]; then
  # C# edits do NOT invalidate sfs_files/shell.mex on their own: the Makefile
  # rule keys off source mtimes, and a hand-run `dotnet build` does not touch
  # them.  Removing the mex forces a real repack so the image can never be
  # tested against a stale managed shell.  (This exact trap cost a full
  # debugging round once -- do not remove this line.)
  rm -f sfs_files/shell.mex
  if ! make -j4 build/os_v2.img > /tmp/fastverify_build.log 2>&1; then
    echo '--- BUILD FAILED ---' >&2
    grep -E 'error|Error' /tmp/fastverify_build.log | head -20 >&2
    exit 1
  fi
fi

if [ ! -f build/os_v2.img ]; then echo 'fastverify: no image' >&2; exit 2; fi

# Guard against the truncated-image trap: an interrupted `cat ... > img` leaves
# a short file that make will happily call up to date, and every conclusion
# drawn from booting it is garbage.  The assembled BIOS image is always at
# least the 4 MiB truncate floor plus the SFS/Linux partitions.
SZ=$(stat -c%s build/os_v2.img)
if [ "$SZ" -lt 4000000 ]; then
  echo "fastverify: build/os_v2.img is only $SZ bytes -- truncated; run 'make' again" >&2
  exit 2
fi

rm -f "$LOG"
qemu-system-i386 -m 512 -drive file=build/os_v2.img,format=raw,if=ide \
  -display none -serial file:"$LOG" -no-reboot &
QP=$!

SEEN=0; i=0; MAX=$(( TIMEOUT * 20 ))
while [ $i -lt $MAX ]; do
  if [ -n "$SENTINEL" ] && grep -qa "$SENTINEL" "$LOG" 2>/dev/null; then SEEN=1; break; fi
  kill -0 $QP 2>/dev/null || break
  i=$((i+1)); sleep 0.05
done
kill $QP 2>/dev/null; wait $QP 2>/dev/null

if [ -n "${GREP:-}" ]; then
  tr -d '\000' < "$LOG" | grep -aE "$GREP"
else
  tr -d '\000' < "$LOG"
fi

if [ -n "$SENTINEL" ] && [ $SEEN -eq 0 ]; then
  echo "fastverify: sentinel not seen: $SENTINEL" >&2; exit 1
fi
exit 0