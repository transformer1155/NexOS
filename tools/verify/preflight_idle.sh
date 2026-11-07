#!/usr/bin/env bash
# ============================================================================
#  preflight_idle.sh - may I start a build or a QEMU run on the build VM?
# ----------------------------------------------------------------------------
#  WHY THIS FILE EXISTS
#    The team standard is "check for a build/QEMU before you start one", but the
#    one-liner kept being written in forms that silently do not work:
#
#      pgrep -a '[m]ake|[c]c1plus|[q]emu-system'
#          -> the '|' is consumed by cmd.exe when the command travels through
#             vm-run.bat, so you get "'[c]c1plus' is not recognized" and never a
#             real answer.  (This bit four different people in one day.)
#
#      pgrep -a 'make|cc1plus'
#          -> the pattern matches the pgrep command line itself, so it ALWAYS
#             reports busy.
#
#      pgrep -c make || echo 0
#          -> pgrep -c prints 0 AND exits non-zero when nothing matches, so the
#             fallback appends a SECOND line and the variable becomes "0\n0",
#             i.e. a busy-looking value with no busy process.
#
#    Three separate calls, bracketed patterns, and no '|| echo 0' are immune to
#    all three.  That is all this script does -- it never starts anything.
#
#  USAGE
#    tools/verify/preflight_idle.sh          # prints each gate, then a verdict
#    tools/verify/preflight_idle.sh && make  # only build if the VM is idle
#    exit code: 0 = idle (safe to start), 1 = busy (wait),
#               3 = cannot tell (pgrep unavailable -- treat as busy, NOT as idle)
#
#  WHAT IT CANNOT SEE
#    pgrep -a matches the process NAME (comm, truncated to 15 chars), not the
#    full command line, so a wrapper such as `bash -c "make ..."` (comm=bash)
#    is missed.  That is acceptable for make / cc1plus / qemu-system, whose own
#    names are distinctive -- do NOT add short or ambiguous patterns here:
#      [l]d  matches kworker/R-kthrotld, kworker/R-mld   (false BUSY)
#      [a]s  matches bash as a SUBSTRING                  (false BUSY)
#    A lock (flock) is the authoritative gate; this script is the fast hint.
#
#  NOTE: this only tells you about *other* processes.  Rules that still apply:
#    one cc1plus at a time; no make while someone is measuring QEMU.
# ============================================================================
set -u

# pgrep itself must exist.  If it does not (or is broken), every gate below would
# look "empty" and the script would print IDLE with rc 0 -- it would lie in the one
# direction that hurts.  Measured: PATH without pgrep -> "RESULT: IDLE", rc=0.
# Exit 3 = cannot tell, and callers must NOT treat that as idle.
command -v pgrep >/dev/null 2>&1 || {
  echo "RESULT: UNKNOWN (pgrep unavailable -- NOT idle, do not start)"
  exit 3
}

busy=0
# A DEFUNCT (zombie) process still matches pgrep -- an exiting QEMU whose parent
# has not reaped it shows up as "qemu-system-x86 <defunct>" and made this script
# report BUSY on a completely idle VM (observed 2026-11-01).  Zombies hold no CPU
# and no lock, so they must never count as busy.  We re-check each match's state.
for pat in '[m]ake' '[c]c1plus' '[q]emu-system'; do
  alive=""
  for pid in $(pgrep "$pat" 2>/dev/null); do
    st=$(ps -o stat= -p "$pid" 2>/dev/null | tr -d ' ')
    case "$st" in
      Z*|"") continue ;;   # zombie (Z) or vanished -> not busy
    esac
    alive="$alive $pid"
  done
  if [ -n "$alive" ]; then
    printf 'BUSY  %-16s pid(s):%s\n' "$pat" "$alive"
    busy=1
  else
    printf 'idle  %-16s\n' "$pat"
  fi
done
if [ "$busy" -eq 0 ]; then
  echo "RESULT: IDLE  (no make / cc1plus / qemu -- safe to start)"
else
  echo "RESULT: BUSY  (wait for the processes above, then re-run)"
fi
exit "$busy"
