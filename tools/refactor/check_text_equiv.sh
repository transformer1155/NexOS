#!/usr/bin/env bash
# ============================================================================
#  check_text_equiv.sh - prove a pure MOVE did not change generated code
# ----------------------------------------------------------------------------
#  WHY THIS EXISTS
#    For a move that stays inside one translation unit, "the .text section is
#    byte-identical" is the right check.  For a CROSS-TU move of data (e.g.
#    moving 'static const' tables into their own .cpp) it is the WRONG check:
#    every access to that data changes from an assembler-resolved section
#    offset to a linker-relocated symbol reference, so a handful of bytes in
#    the instruction stream necessarily change -- the instruction ENCODINGS do
#    not.  Those changed bytes are exactly the 4-byte relocation fields the
#    linker fills in.
#
#    So we verify what actually matters: instruction stream unchanged, and the
#    only differing bytes are relocation sites.
#
#  CHECKS (all must pass)
#    1. .text size equal
#    2. relocation entry count equal
#    3. 100% of differing .text bytes fall inside a 4-byte relocation field
#       (checked in BOTH objects; a diff outside one means real code changed)
#    4. disassembly opcode stream identical (operands stripped)
#
#  USAGE
#    tools/refactor/check_text_equiv.sh <baseline.o> <current.o>
#      exit 0 = PASS, 1 = FAIL, 2 = usage/tool error
#
#  TYPICAL USE after a move:
#    git show HEAD:src/gui/gui_legacy.cpp > /tmp/head.cpp
#    printf '#include "/tmp/head.cpp"\n' > /tmp/head_entry.cpp
#    g++ <same flags as the Makefile> -c /tmp/head_entry.cpp -o /tmp/base.o
#    make build/gui.o
#    tools/refactor/check_text_equiv.sh /tmp/base.o build/gui.o
#
#  NOTE: after ANY move also run 'make check-k64'.  That gate (task-15) does not
#  only catch symbols bound to 0 -- during step 2a-1 it also caught three real
#  linkage mistakes: an object missing from the link line, a rule line that had
#  accidentally become a multi-target rule, and prerequisites appended after
#  '|' (order-only, so edits no longer triggered a rebuild).
# ============================================================================
set -u
BASE="${1:-}"; CUR="${2:-}"
[ -n "$BASE" ] && [ -n "$CUR" ] || { echo "usage: $0 <baseline.o> <current.o>" >&2; exit 2; }
[ -f "$BASE" ] || { echo "check_text_equiv: no such file: $BASE" >&2; exit 2; }
[ -f "$CUR"  ] || { echo "check_text_equiv: no such file: $CUR"  >&2; exit 2; }

TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
fail=0
say() { printf '%s\n' "$*"; }

# ---- 1. sizes -------------------------------------------------------------
objcopy -O binary --only-section=.text "$BASE" "$TMP/b.bin" 2>/dev/null || { echo "no .text in $BASE" >&2; exit 2; }
objcopy -O binary --only-section=.text "$CUR"  "$TMP/c.bin" 2>/dev/null || { echo "no .text in $CUR"  >&2; exit 2; }
sb=$(stat -c%s "$TMP/b.bin"); sc=$(stat -c%s "$TMP/c.bin")
if [ "$sb" = "$sc" ]; then say "  [1] .text size          : $sb = $sc  OK"
else say "  [1] .text size          : $sb vs $sc  FAIL"; fail=1; fi

# ---- 2. relocation counts -------------------------------------------------
readelf -r "$BASE" 2>/dev/null | grep -cE "R_(386|X86_64)_" > "$TMP/rb.n" || true
readelf -r "$CUR"  2>/dev/null | grep -cE "R_(386|X86_64)_" > "$TMP/rc.n" || true
nb=$(cat "$TMP/rb.n"); nc=$(cat "$TMP/rc.n")
if [ "$nb" = "$nc" ]; then say "  [2] relocations         : $nb = $nc  OK"
else say "  [2] relocations         : $nb vs $nc  FAIL"; fail=1; fi

# ---- 3. every differing byte must be inside a relocation field -----------
readelf -r "$BASE" 2>/dev/null | grep -E "R_(386|X86_64)_" | awk '{print $1}' | sed 's/^0*//' > "$TMP/rb.txt"
readelf -r "$CUR"  2>/dev/null | grep -E "R_(386|X86_64)_" | awk '{print $1}' | sed 's/^0*//' > "$TMP/rc.txt"
cmp -l "$TMP/b.bin" "$TMP/c.bin" 2>/dev/null | awk '{print $1-1}' > "$TMP/diff.txt" || true
if python3 - "$TMP/rb.txt" "$TMP/rc.txt" "$TMP/diff.txt" <<'PY'
import sys
def rel(p):
    s=set()
    for line in open(p):
        line=line.strip()
        if line: s.add(int(line,16))
    return s
rb, rc = rel(sys.argv[1]), rel(sys.argv[2])
diff=[int(x) for x in open(sys.argv[3]) if x.strip()]
def covered(off, R):
    for r in R:
        if r <= off < r+4: return True   # R_386_32/PC32, R_X86_64_32(S)/PC32/PLT32 = 4 bytes
    return False
bad_b=[o for o in diff if not covered(o, rb)]
bad_c=[o for o in diff if not covered(o, rc)]
print("  [3] differing bytes     : %d total; covered by reloc field: HEAD %d/%d, CUR %d/%d  %s" %
      (len(diff), len(diff)-len(bad_b), len(diff), len(diff)-len(bad_c), len(diff),
       "OK" if not bad_b and not bad_c else "FAIL"))
if bad_b or bad_c:
    for o in (bad_b+bad_c)[:5]: print("        uncovered byte at 0x%x" % o)
    sys.exit(1)
PY
then :; else fail=1; fi

# ---- 4. disassembly opcode stream identical ------------------------------
# Compare instruction SEQUENCE: mnemonic + encoded length per instruction.
# Operand bytes are deliberately NOT compared here -- displacement fields are
# covered by check [3]; comparing them here would just re-report the relocations.
dis() {
  objdump -d "$1" 2>/dev/null | sed -n 's/^ *[0-9a-f]*:\t\([0-9a-f ]*\)\t *\([a-z][a-z0-9.]*\).*/\1|\2/p' |
    awk -F'|' '{ n=split($1, a, " "); print $2, n }'
}
dis "$BASE" > "$TMP/db.txt"; dis "$CUR" > "$TMP/dc.txt"
if cmp -s "$TMP/db.txt" "$TMP/dc.txt"; then say "  [4] opcode stream       : identical  OK"
else
  say "  [4] opcode stream       : DIFFERS  FAIL"
  diff "$TMP/db.txt" "$TMP/dc.txt" | head -5 | sed 's/^/        /'
  fail=1
fi

say ""
if [ "$fail" -eq 0 ]; then
  say "RESULT: PASS  (instruction stream unchanged; differing bytes are relocation sites only)"
  exit 0
else
  say "RESULT: FAIL  (this is NOT a pure move -- investigate before committing)"
  exit 1
fi
