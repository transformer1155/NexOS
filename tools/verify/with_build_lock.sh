#!/usr/bin/env bash
# ============================================================================
#  with_build_lock.sh - serialise CPU-heavy build steps on the shared VM.
# ----------------------------------------------------------------------------
#  WHY A LOCK AND NOT ANOTHER CHECK
#    preflight_idle.sh tells you whether the VM looks idle *right now*, but
#    two agents can both read IDLE and both start - check-then-act is a race.
#    This script does not check: it takes an exclusive lock, so the second
#    caller BLOCKS instead of colliding. A lock also cannot be fooled by
#    process-name matching (pgrep -a '[l]d' matches kworker/R-kthrotld on an
#    idle box; [a]s matches bash; [g]++ matches any name containing a g).
#
#  SCOPE: the lock must be held by EVERY CPU-heavy step, not just make,
#    because a direct compiler invocation burns the same single vCPU:
#      with_build_lock.sh make -j2
#      with_build_lock.sh g++ -c foo.cpp -o /tmp/foo.o
#      with_build_lock.sh g++ -T rt/link.ld -o /tmp/x.elf ...
#    QEMU runs take the SAME lock as builds (/tmp/nexos_vm.lock, used by
#    vm-run.bat for make AND for test/test-log), so a build and a measurement
#    are mutually exclusive - not merely separately serialised. That matters
#    because cc1plus alone skews the TSC calibration by ~2.6x, i.e. it does not
#    just slow a measurement down, it makes the numbers wrong.
#
#  USAGE
#    tools/verify/with_build_lock.sh <command> [args...]
#      exit code = the command's own exit code
#      1 and "flock: failed to get lock" means the wait expired: someone else
#      held it for NEXOS_BUILD_LOCK_WAIT seconds (default 600).
#    NEXOS_VM_LOCK_WAIT=1200 to wait longer (legacy NEXOS_BUILD_LOCK_WAIT still honoured).
# ============================================================================
set -u
LOCK="${NEXOS_VM_LOCK:-${NEXOS_BUILD_LOCK:-/tmp/nexos_vm.lock}}"
WAIT="${NEXOS_VM_LOCK_WAIT:-${NEXOS_BUILD_LOCK_WAIT:-600}}"

if [ "$#" -eq 0 ]; then
  echo "usage: with_build_lock.sh <command> [args...]" >&2
  echo "       serialises the command under $LOCK (wait ${WAIT}s)" >&2
  exit 2
fi

if command -v flock >/dev/null 2>&1; then
  # `flock -w N` exits 1 on timeout and prints NOTHING, so a caller would see a
  # bare rc=1 and could not tell "the command failed" from "the lock expired".
  # Probe non-blockingly first and announce the wait; the exit code stays flock's.
  if ! flock -n "$LOCK" true 2>/dev/null; then
    echo "with_build_lock: $LOCK is held - waiting up to ${WAIT}s (build or QEMU run in progress)" >&2
  fi
  # flock runs the command itself and propagates its exit code.
  exec flock -w "$WAIT" "$LOCK" "$@"
fi

# No flock available: fail LOUDLY rather than silently running unlocked,
# because a silent downgrade is exactly how a guard becomes theatre.
echo "with_build_lock: flock not found - refusing to run unlocked." >&2
echo "  (install util-linux / util-linux-extra, or run the command deliberately" >&2
echo "   after tools/verify/preflight_idle.sh reports IDLE.)" >&2
exit 3