#!/usr/bin/env bash
# assert_artifact_changed.sh -- prove a build actually rewrote its artifact.
#
# WHY: `make` can stop at a gate (e.g. check-k64) AFTER some earlier step already
# wrote output, so the build 'looks like it ran' while the artifact you then boot
# or measure is the OLD one.  This has bitten the team three times (a new kernel
# with the old mex, an un-compiled change, and a gate abort leaving a stale bin).
# Every such incident produced a confidently wrong conclusion from a stale file.
#
# Usage:
#   assert_artifact_changed.sh --record <path> <baseline_file>
#   assert_artifact_changed.sh --check  <path> <baseline_file>
#
# Exit codes (deliberately distinct -- 'cannot verify' must never read as 'failed',
# and 'not rebuilt' must never read as 'ok'):
#   0  rebuilt (sha changed), or --record succeeded
#   1  usage error
#   3  ARTEFACT NOT REBUILT  (sha same, and not (size and mtime both changed))
#   4  artifact missing
#   5  artifact empty or all-zero
#   6  baseline missing (new artifact -- record it first)
#
# Path standard: repository root from $0; no absolute paths.
#
# SELF-TEST lives in _selftest_artifact_changed.sh, DELIBERATELY not in this file:
# a judge must not be part of the player.  If the self-test sat inside here,
# 'it passed' could only mean two pieces of code agree -- possibly on the same
# wrong assumption.  A separate file guarantees we are testing another thing.
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"

usage() { echo "usage: $0 --record|--check <path> <baseline_file>" >&2; exit 1; }
[ $# -eq 3 ] || usage
MODE="$1"; ART="$2"; BASE="$3"
case "$MODE" in --record|--check) ;; *) usage;; esac

# absolute-ise a relative path against the repo root so callers can pass either
case "$ART" in /*) ;; *) ART="$ROOT/$ART";; esac
case "$BASE" in /*) ;; *) BASE="$ROOT/$BASE";; esac

if [ ! -f "$ART" ]; then echo "ARTEFACT MISSING: $ART"; exit 4; fi
SZ=$(stat -c%s "$ART")
if [ "$SZ" -eq 0 ]; then echo "ARTEFACT EMPTY: $ART"; exit 5; fi
NONZERO=$(tr -d '\000' < "$ART" | wc -c | tr -d ' ')
if [ "$NONZERO" -eq 0 ]; then echo "ARTEFACT ALL-ZERO ($SZ bytes): $ART"; exit 5; fi
SHA=$(sha256sum "$ART" | cut -c1-32)
MT=$(stat -c %Y "$ART")
LINE="$SZ $MT $SHA"

if [ "$MODE" = "--record" ]; then
  printf '%s\n' "$LINE" > "$BASE"
  echo "RECORDED $ART  size=$SZ mtime=$MT sha=${SHA:0:16}"
  exit 0
fi

if [ ! -f "$BASE" ]; then echo "BASELINE MISSING: $BASE (new artifact -- record it first)"; exit 6; fi
OLD=$(head -1 "$BASE")
if [ "$OLD" = "$LINE" ]; then
  echo "ARTEFACT NOT REBUILT -- refusing to use it"
  echo "  $ART unchanged (size=$SZ mtime=$MT sha=${SHA:0:16})"
  echo "  a gate may have aborted the build after an earlier step wrote this file"
  exit 3
fi
OSZ=${OLD%% *}; REST=${OLD#* }; OMT=${REST%% *}
echo "REBUILT  $ART"
echo "  size $OSZ -> $SZ   mtime $OMT -> $MT   sha $(printf %.16s "${OLD##* }")"
exit 0
