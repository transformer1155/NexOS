#!/usr/bin/env bash
# ============================================================================
#  check_k64_undefined.sh - build-time gate against silent zero-bound symbols
# ----------------------------------------------------------------------------
#  WHY THIS EXISTS
#    kernel64 links with --unresolved-symbols=ignore-all.  A symbol the link
#    cannot satisfy is bound to address 0, and the FIRST time anything calls it
#    the CPU jumps to 0, runs the real-mode IVT bytes and triple-faults -- so
#    the symptom is a "random reboot", hours away from its cause.
#
#    Two of these cost real days (task-8):
#      serial_enter / serial_leave -> 64-bit kernel died inside fb_console_init()
#      bigbuf_alloc                -> 64-bit gui_init() died on its backbuffer
#
#  WHAT IT DOES
#    1. Re-links kernel64 WITHOUT ignore-all into a scratch file, so the
#       linker itself names every symbol it could not satisfy.
#    2. Compares that set against tools/k64_undefined_allowlist.txt.
#    3. FAILS (exit 1) on any symbol that is not allow-listed, i.e. a NEW
#       landmine.  Allow-listed ones are reported as a burn-down reminder, and
#       allow-list entries that are now resolved are reported as stale.
#
#  The Makefile passes K64_OBJS / K64_LDFLAGS in, so the object list has one
#  source of truth.  Skip a one-off run with: SKIP_K64_CHECK=1 make ...
# ============================================================================
set -u
cd "$(dirname "$0")/.." || exit 1

OBJS="${K64_OBJS:-}"
STRICT_FLAGS="${K64_LDFLAGS:-}"
ALLOW="tools/k64_undefined_allowlist.txt"
SCRATCH="${K64_STRICT_ELF:-/tmp/nexos_k64_strict.elf}"

if [ -z "$OBJS" ]; then
  echo "check_k64_undefined: K64_OBJS not set (run it through make)" >&2; exit 2
fi
if [ ! -f "$ALLOW" ]; then
  echo "check_k64_undefined: missing $ALLOW" >&2; exit 2
fi

RUNDIR=$(mktemp -d)
trap 'rm -rf "$RUNDIR"' EXIT

# --- 1) strict re-link -------------------------------------------------------
# shellcheck disable=SC2086
ld $STRICT_FLAGS -o "$SCRATCH" $OBJS 2> "$RUNDIR/ld.err"
LD_RC=$?

# ld quotes symbol names with a backtick; match any leading char to avoid
# embedding it, then strip the trailing quote.
grep -oE "undefined reference to .[^']*'" "$RUNDIR/ld.err" \
  | sed "s/undefined reference to .//" | sed "s/'$//" | sort -u > "$RUNDIR/undef.txt" || true

# allowlist: strip comments and blank lines
sed -e "s/#.*//" -e "s/[[:space:]]*$//" "$ALLOW" | grep -v "^$" | sort -u > "$RUNDIR/allow.txt"

NEW=$(comm -23 "$RUNDIR/undef.txt" "$RUNDIR/allow.txt")
STALE=$(comm -13 "$RUNDIR/undef.txt" "$RUNDIR/allow.txt")
COUNT=$(wc -l < "$RUNDIR/undef.txt" | tr -d " ")

if [ "$COUNT" != "0" ]; then
  echo "    k64 undefined: $COUNT symbol(s) bound to 0 (allow-listed burn-down list)"
else
  echo "    k64 undefined: 0 (clean)"
fi

if [ -n "$STALE" ]; then
  echo "    note: allowlist entries that are now resolved -- delete these lines:"
  echo "$STALE" | sed "s/^/        /"
fi

if [ -n "$NEW" ]; then
  echo "" >&2
  echo "*** ERROR: kernel64 binds these symbols to address 0 and they are NOT allow-listed:" >&2
  echo "$NEW" | sed "s/^/***        /" >&2
  echo "***" >&2
  echo "***   Calling one jumps to 0 -> #UD -> #GP -> #DF -> triple fault." >&2
  echo "***   Exactly the failure that cost two days in task-8 (serial_enter," >&2
  echo "***   then bigbuf_alloc).  Define the symbol for the 64-bit kernel, or" >&2
  echo "***   add it to tools/k64_undefined_allowlist.txt with a note on when it gets implemented." >&2
  exit 1
fi

# The strict link is EXPECTED to fail while any symbol is undefined: that is
# precisely how the linker tells us their names.  Only treat a failure as an
# error when it produced no undefined-symbol diagnostics at all (a real link
# problem: bad object, missing file, wrong flags ...).
if [ "$LD_RC" -ne 0 ] && [ "$COUNT" = "0" ]; then
  echo "*** ERROR: strict kernel64 re-link failed without naming any undefined" >&2
  echo "***        symbol -- that is a real link problem, not a bind-to-zero one." >&2
  echo "***        Reproduce with: ld $STRICT_FLAGS -o /tmp/k64.elf $OBJS" >&2
  exit 1
fi
exit 0
