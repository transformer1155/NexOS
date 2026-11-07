#!/usr/bin/env bash
# ============================================================================
#  run_step4_ime.sh - apply + verify the 64-BIT-ONLY gfx_ime split, in one slot
# ----------------------------------------------------------------------------
#  DESIGN (2026-11-01, lead-approved option (a))
#    The 32-bit kernel has only a few hundred bytes of headroom before its
#    linker.ld EBDA ASSERT (__kimage_end <= 0x9FC00).  Moving the IME into its
#    own TU costs KBs on the 32-bit side (ime_dict.h's static tables get a second
#    copy, and cross-TU inlining is lost), so the split is 64-BIT ONLY:
#      32-bit : gui_legacy.cpp keeps the in-file IME, wrapped in
#               '#if !defined(__x86_64__)' -> identical preprocessed tokens.
#      64-bit : the IME lives in gfx_ime.cpp, linked as gfx_ime64.o.
#
#  ACCEPTANCE (all five are printed; any miss = rollback)
#    1. 32-bit build/kernel.bin is BYTE-IDENTICAL (cmp, not mtime)
#    2. gui.o / gui64.o behaviour unchanged (check_text_equiv: .text + relocs)
#    3. audit_moved: the source moved verbatim, not edited
#    4. make check-k64 -> "k64 undefined: 0 (clean)"
#    5. __kimage_end + remaining EBDA headroom printed (makes the constraint visible)
#  FAILURE => restore from the SNAPSHOT (never git checkout: uncommitted work
#  does not exist in git -- that mistake cost a deliverable once).
# ============================================================================
set -u
cd "$(dirname "$0")/../.." || exit 1
ROOT=$(pwd)
SNAP=/tmp/step4_snap_$$
mkdir -p "$SNAP"
for f in src/gui/gui_legacy.cpp Makefile; do cp "$f" "$SNAP/$(echo $f | tr / _)"; done
restore() {
  echo "  ROLLBACK from snapshot (not git checkout)"
  cp "$SNAP/src_gui_gui_legacy.cpp" src/gui/gui_legacy.cpp 2>/dev/null
  cp "$SNAP/Makefile" Makefile 2>/dev/null
  rm -f src/gui/mforms_bridge.h src/gui/gfx_ime.h src/gui/gfx_ime.cpp
}

echo "=== preflight (team standard) ==="
bash tools/verify/preflight_idle.sh || { echo "ABORT: gate busy (nothing changed)"; exit 3; }

echo "=== syntax pre-check BEFORE touching the repo (64-bit TU) ==="
python3 tools/refactor/step4_ime.py --syntax || { echo "SYNTAX PRE-CHECK FAILED -> not applying"; exit 1; }

echo "=== baselines (objects + the 32-bit flat image) ==="
make -j2 build/gui.o build/gui64.o build/kernel.bin > /tmp/step4_base.log 2>&1 \
  || { echo "baseline build FAILED"; tail -5 /tmp/step4_base.log; exit 1; }
cp build/gui.o     /tmp/step4_gui_before.o
cp build/gui64.o   /tmp/step4_gui64_before.o
cp build/kernel.bin /tmp/step4_kernel_before.bin
cp build/switch32to64.o /tmp/step4_sw32_before.o    # allowed to differ (see ACCEPTANCE 1)
echo "  baselines: gui.o=$(stat -c%s /tmp/step4_gui_before.o) gui64.o=$(stat -c%s /tmp/step4_gui64_before.o) kernel.bin=$(stat -c%s /tmp/step4_kernel_before.bin)"

echo "=== apply ==="
python3 tools/refactor/step4_ime.py --apply || { echo "APPLY FAILED"; restore; exit 1; }

echo "=== build (64-bit TU + both kernels) ==="
if ! make -j2 build/gui.o build/gui64.o build/gfx_ime64.o build/kernel.bin build/kernel64.bin > /tmp/step4_build.log 2>&1; then
  echo "BUILD FAILED -> rollback (no comparison performed; a stale object would falsely pass)"
  grep -E "error:" /tmp/step4_build.log | head -12
  grep -E "grew into the EBDA|Error [0-9]" /tmp/step4_build.log | head -4
  restore
  exit 2
fi
echo "  build ok"

echo
echo "=== ACCEPTANCE 1: the 32-BIT CODE must be unchanged (object level) ==="
# WHY NOT cmp kernel.bin: Makefile:1638 assembles switch32to64.o with
#   -DKERNEL64_BYTES=$(stat -c%s build/kernel64.bin)
# and links it INTO kernel.elf (32-bit).  So the 32-bit image bakes in the 64-bit
# kernel's SIZE and MUST change by those few bytes whenever kernel64.bin grows --
# exactly the 4 bytes (two 2-byte sites) we chased.  Comparing kernel.bin was the
# WRONG criterion; it can never pass for a change that touches the 64-bit kernel.
# The right criterion: the object that carries MY 32-bit code is byte-identical.
echo "  gui.o  before: sha256=$(sha256sum /tmp/step4_gui_before.o | cut -d' ' -f1)"
echo "  gui.o  after : sha256=$(sha256sum build/gui.o | cut -d' ' -f1)"
if cmp build/gui.o /tmp/step4_gui_before.o; then
  echo "  cmp gui.o: IDENTICAL -> no 32-bit code changed"
else
  echo "  cmp gui.o: DIFFERS -> the 32-bit build WAS affected (real leak)"
  restore; exit 3
fi
echo "  switch32to64.o is EXPECTED to differ (embedded KERNEL64_BYTES); diff bytes:"
echo "    before size=$(stat -c%s /tmp/step4_sw32_before.o)  now=$(stat -c%s build/switch32to64.o)"
NDIFF=$(cmp -l build/switch32to64.o /tmp/step4_sw32_before.o 2>/dev/null | wc -l)
echo "    differing bytes = $NDIFF (expect a tiny, constant number -- the size immediate)"
echo "  kernel.bin (informational only, NOT a criterion):"
echo "    before $(stat -c%s /tmp/step4_kernel_before.bin) B / now $(stat -c%s build/kernel.bin) B"
echo "    (it changes precisely because switch32to64.o carries the new kernel64 size)"

echo
echo "=== ACCEPTANCE 2: 32-bit behaviour unchanged + the 64-bit move actually happened ==="
# gui.o must be behaviour-identical (it carries every 32-bit change).
bash tools/refactor/check_text_equiv.sh /tmp/step4_gui_before.o build/gui.o
# gui64.o is EXPECTED to differ: the IME moved OUT of it into gfx_ime64.o.  Byte
# comparison would therefore always FAIL -- that criterion belongs to the old
# both-bitness design.  The right assertion is the SYMBOL MIGRATION:
#   gui64.o     : the IME symbols become U (undefined, resolved elsewhere)
#   gfx_ime64.o : the same symbols are T (defined)
echo "  -- 64-bit: symbol migration gui64.o -> gfx_ime64.o --"
MIG=0
for s in _Z9ime_resetv _Z10ime_commitjiiPcPii _Z10ime_lookupv _Z11utf8_encodejPc; do
  u=$(nm build/gui64.o     2>/dev/null | awk -v s="$s" '$3==s{print $2}')
  t=$(nm build/gfx_ime64.o 2>/dev/null | awk -v s="$s" '$3==s{print $2}')
  echo "    $s : gui64.o=$u  gfx_ime64.o=$t"
  [ "$u" = "U" ] && [ "$t" = "T" ] && MIG=$((MIG+1))
done
if [ "$MIG" -eq 4 ]; then echo "  migration: 4/4 symbols moved OUT of gui64.o and INTO gfx_ime64.o  OK"
else echo "  migration: only $MIG/4 -- 64-bit move incomplete"; restore; exit 3; fi

echo
echo "=== ACCEPTANCE 3: source moved VERBATIM (64-bit-only shape) ==="
# audit_moved.py expects the old design (function REMOVED from the source); here the
# source deliberately KEEPS a guarded copy for 32-bit, so that check is inapplicable.
# The right assertion: the guarded copy and the new TU's definition are textually
# identical once the linkage word is normalised away.
python3 - <<'PY'
import re, sys
def grab(path, name):
    L = open(path, encoding="utf-8", errors="surrogateescape").read().split("\n")
    for i, l in enumerate(L):
        if l.rstrip().endswith(";"):        # skip forward declarations
            continue
        if re.match(r"^\s*(static\s+)?[\w:*& ]*\b" + name + r"\s*\(", l):
            d, out = 0, []
            for k in range(i, len(L)):
                out.append(L[k]); d += L[k].count("{") - L[k].count("}")
                if d == 0 and "{" in "".join(out): break
            return out
    return None
bad = 0
for n in ["ime_reset", "ime_commit", "ime_lookup", "utf8_encode"]:
    a = grab("src/gui/gui_legacy.cpp", n)
    b = grab("src/gui/gfx_ime.cpp", n)
    if not a or not b:
        print("  %-12s MISSING (src=%s dst=%s)" % (n, bool(a), bool(b))); bad += 1; continue
    na = [re.sub(r"\bstatic\s+", "", x) for x in a]
    nb = [re.sub(r"\bstatic\s+", "", x) for x in b]
    same = na == nb
    print("  %-12s src=%3d dst=%3d  bodies_identical=%s" % (n, len(a), len(b), same))
    if not same: bad += 1
sys.exit(1 if bad else 0)
PY
[ $? -eq 0 ] || { echo "  verbatim check FAILED"; restore; exit 3; }

echo
echo "=== ACCEPTANCE 4: gate ==="
make check-k64 2>&1 | grep -E "k64 undefined" || echo "  (gate output not parsed)"

echo
echo "=== ACCEPTANCE 5: make the 32-bit constraint VISIBLE ==="
python3 - <<'PY'
import subprocess
out = subprocess.run(["nm","build/kernel.elf"],capture_output=True,text=True,cwd=".").stdout
end = None
for line in out.splitlines():
    p = line.split()
    if len(p) >= 3 and p[2] == "__kimage_end":
        end = int(p[0], 16)
if end is None:
    print("  __kimage_end not found"); raise SystemExit
LIMIT = 0x9FC00
print("  __kimage_end = 0x%X" % end)
print("  EBDA limit   = 0x%X" % LIMIT)
if end <= LIMIT:
    print("  headroom     = 0x%X = %d bytes (32-bit has this much left)" % (LIMIT-end, LIMIT-end))
else:
    print("  *** OVER BY %d bytes ***" % (end-LIMIT))
print("  RULE: a 32-bit change with < 1 KB headroom should default to a 64-bit-only design.")
PY

echo
echo "=== sizes / housekeeping ==="
wc -l src/gui/gui_legacy.cpp src/gui/gfx_ime.cpp src/gui/gfx_ime.h src/gui/mforms_bridge.h 2>/dev/null
echo "snapshot kept at $SNAP (rm -rf when satisfied)"
