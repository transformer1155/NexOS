#!/usr/bin/env bash
# _selftest_artifact_changed.sh -- 3 positives + 4 negatives, each asserting
# exit code AND the reason substring in the output.
#
# WHY THE DOUBLE ASSERTION: the first version of a sibling self-test PASSED for
# the wrong reason -- an argument-parsing bug sent every call down the usage
# branch, and since usage also exited 3 the negative cases 'all passed'.  A test
# that only checks an exit code cannot tell 'detected the right thing' from
# 'failed for an unrelated reason'.  So every case below checks both.
#
# Semantic case names are shared across the self-tests in this directory:
#   CE_ALLZERO / CE_BADMAGIC / CE_NEWENTRY / CE_CHANGED / CE_SAMESIZE / CE_NOARGS
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
S="$ROOT/tools/verify/assert_artifact_changed.sh"
T="/tmp/selftest_aac_$$"; mkdir -p "$T"
P=0; F=0

ck() { # ck <case> <want_rc> <got_rc> <output> <want_substr>
  local ok=1
  [ "$2" = "$3" ] || ok=0
  grep -qF "$5" <<<"$4" || ok=0
  if [ "$ok" = "1" ]; then echo "  PASS  $1"; P=$((P+1));
  else
    echo "  FAIL  $1 (want rc=$2 got rc=$3; want text '$5')"
    echo "        out: $(echo "$4" | head -2 | tr '\n' '|')"
    F=$((F+1))
  fi
}

echo "=== assert_artifact_changed self-test (3 positives + 4 negatives) ==="

# --- POSITIVE 1: a real rebuild (bigger + newer + new sha) -> 0
A="$T/build.bin"; printf 'AAAA' > "$A"; B="$T/base.txt"
bash "$S" --record "$A" "$B" >/dev/null 2>&1
printf 'BBBBBB' > "$A"; touch -d '+2 seconds' "$A" 2>/dev/null || true
OUT=$(bash "$S" --check "$A" "$B" 2>&1); rc=$?
ck "P_REBUILT" 0 $rc "$OUT" "REBUILT"

# --- POSITIVE 2: SAME SIZE but different content -> must still be seen as rebuilt
#     (a rebuild can produce an identical byte count; judging only on size fails.)
C="$T/same.bin"; printf 'CCCC' > "$C"; BC="$T/bc.txt"
bash "$S" --record "$C" "$BC" >/dev/null 2>&1
printf 'DDDD' > "$C"; touch -d '+2 seconds' "$C" 2>/dev/null || true
OUT=$(bash "$S" --check "$C" "$BC" 2>&1); rc=$?
ck "P_SAMESIZE" 0 $rc "$OUT" "REBUILT"

# --- NEGATIVE CE_UNCHANGED: identical file -> 3 + refusal
#     Re-record first: the previous case changed the file, so the stored
#     fingerprint is stale and a check would correctly say REBUILT.  (First
#     version of this test forgot that and blamed the checker.)
bash "$S" --record "$C" "$BC" >/dev/null 2>&1
OUT=$(bash "$S" --check "$C" "$BC" 2>&1); rc=$?
ck "CE_UNCHANGED" 3 $rc "$OUT" "ARTEFACT NOT REBUILT"

# --- NEGATIVE CE_MISSING: artifact absent -> 4 (NOT 3: cannot verify != not rebuilt)
OUT=$(bash "$S" --check "$T/nope.bin" "$B" 2>&1); rc=$?
ck "CE_MISSING" 4 $rc "$OUT" "ARTEFACT MISSING"

# --- NEGATIVE CE_ALLZERO: all-zero file -> 5
Z="$T/zero.bin"; head -c 4096 /dev/zero > "$Z"; bash "$S" --record "$Z" "$T/bz.txt" >/dev/null 2>&1
OUT=$(bash "$S" --check "$Z" "$T/bz.txt" 2>&1); rc=$?
ck "CE_ALLZERO" 5 $rc "$OUT" "ALL-ZERO"

# --- NEGATIVE CE_NOBASELINE: baseline absent (new artifact) -> 6
OUT=$(bash "$S" --check "$A" "$T/no_such_baseline" 2>&1); rc=$?
ck "CE_NOBASELINE" 6 $rc "$OUT" "BASELINE MISSING"

# --- NEGATIVE CE_NOARGS: no arguments -> 1 usage, and NOT 3
OUT=$(bash "$S" 2>&1); rc=$?
ck "CE_NOARGS" 1 $rc "$OUT" "usage"

echo; echo "=== selftest: $P passed, $F failed ==="
rm -rf "$T"
[ "$F" -eq 0 ] || exit 1
exit 0
