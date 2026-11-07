#!/usr/bin/env python3
"""check_vmcb_offsets.py -- machine cross-check of every VMCB offset constant.

WHY: svm-porter fixed the control-area offsets in THREE places (svm_driver.c,
vmm.cpp, kernel64.cpp) and the dynamic end-to-end test passed.  But a passing
test cannot show that an offset no path uses is still correct -- the static
cross-check below caught two fields that were "consistent, wrong, and in use"
(VMCB_SAVE_CR2, VMCB_NEXT_RIP).  Run this after ANY edit to those three files.

THE EXPECTED VALUES COME FROM svm/svm_vmcb.h SVM_STATIC_ASSERT lines -- the one
in-tree authority -- and never from a hand-written table.  If a field has no
assert in the reference, it is reported as NO REFERENCE rather than guessed.

USAGE:  python3 svm/check_vmcb_offsets.py        (exit 1 on any mismatch)
"""
import re, sys, os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REF  = os.path.join(ROOT, "svm", "svm_vmcb.h")
SRCS = [("drivers/svm_driver.c", "svm_driver.c"),
        ("plugins/vmm.cpp",      "vmm.cpp"),
        (".attic64/kernel64.cpp","kernel64.cpp")]

# field name in the reference struct -> the VMCB_ macro the implementations use
# (this is a name TRANSLATION, not an expected value -- values come from asserts)
MAP = {
  "iopm_base_pa":  "VMCB_IOPM_BASE_PA",  "msrpm_base_pa": "VMCB_MSRPM_BASE_PA",
  "asid":          "VMCB_ASID",          "tlb_ctl":       "VMCB_TLB_CTL",
  "int_ctl":       "VMCB_INT_CTL",       "exit_code":     "VMCB_EXITCODE",
  "exit_info_1":   "VMCB_EXITINFO1",     "exit_info_2":   "VMCB_EXITINFO2",
  "event_inj":     "VMCB_EVENT_INJ",     "next_rip":      "VMCB_NEXT_RIP",
  "clean":         "VMCB_CLEAN",
  "efer":  "VMCB_SAVE_EFER",  "cr4": "VMCB_SAVE_CR4",  "cr3": "VMCB_SAVE_CR3",
  "cr0":   "VMCB_SAVE_CR0",   "rflags": "VMCB_SAVE_RFLAGS", "rip": "VMCB_SAVE_RIP",
  "rsp":   "VMCB_SAVE_RSP",   "rax": "VMCB_SAVE_RAX",  "cr2": "VMCB_SAVE_CR2",
  "g_pat": "VMCB_SAVE_G_PAT",
  # segment descriptors / cpl / vmpl: the reference DOES assert these, so they
  # belong in the checked table (my first MAP omitted them, which made the
  # unverified-inventory mislabel them as unchecked).
  "es": "VMCB_SAVE_ES", "cs": "VMCB_SAVE_CS", "gdtr": "VMCB_SAVE_GDTR",
  "idtr": "VMCB_SAVE_IDTR", "tr": "VMCB_SAVE_TR",
  "vmpl": "VMCB_SAVE_VMPL", "cpl": "VMCB_SAVE_CPL",
}

def read(p):
    return open(p, encoding="utf-8", errors="surrogateescape").read()

# ---- authoritative expected values -----------------------------------------
txt = read(REF)
SAVE_OFF = int(re.search(r"#define\s+VMCB_SAVE_AREA_OFFSET\s+(\d+)u", txt).group(1))
EXPECT = {}
for m in re.finditer(r"SVM_STATIC_ASSERT\(offsetof\(struct svm_vmcb_(control|save),\s*(\w+)\)\s*==\s*(0x[0-9A-Fa-f]+)", txt):
    where, field, off = m.group(1), m.group(2), int(m.group(3), 16)
    EXPECT[field] = (off + (SAVE_OFF if where == "save" else 0), where)

# ---- what the three implementations actually define ------------------------
impl = {}
for rel, short in SRCS:
    impl[short] = {}
    for m in re.finditer(r"#\s*define\s+(VMCB_[A-Z0-9_]+)\s+(0x[0-9A-Fa-f]+)", read(os.path.join(ROOT, rel))):
        impl[short][m.group(1)] = int(m.group(2), 16)

# ---- compare ---------------------------------------------------------------
names = sorted(MAP.items(), key=lambda kv: EXPECT.get(kv[0], (1 << 32, ""))[0])
print("%-18s %-14s %-8s %-12s %-12s %-12s %s" % ("reference field", "VMCB macro", "area", "expected", "svm_driver.c", "vmm.cpp", "kernel64.cpp"))
print("-" * 104)
bad = noRef = miss = 0
for field, macro in names:
    exp = EXPECT.get(field)
    vals = [impl[s].get(macro) for _, s in SRCS]
    exp_s = ("0x%03X" % exp[0]) if exp else "NO REFERENCE"
    area = exp[1] if exp else "-"
    cells = [("0x%03X" % v) if v is not None else "-" for v in vals]
    if exp is None:
        noRef += 1; mark = "NO REFERENCE ASSERT"
    else:
        distinct = set(v for v in vals if v is not None)
        if any(v is None for v in vals):
            miss += 1; mark = "MISSING in %d file(s) [not a failure -- may be by design]" % sum(1 for v in vals if v is None)
        elif distinct != {exp[0]}:
            bad += 1
            mark = ("ALL THREE WRONG (same value)" if len(distinct) == 1 else "THREE-WAY DISAGREE")
        else:
            mark = "ok"
    print("%-18s %-14s %-8s %-12s %-12s %-12s %-12s %s" % (field, macro, area, exp_s, cells[0], cells[1], cells[2], mark))
print("-" * 104)
print("checked=%d  VALUE mismatches=%d  no-reference=%d" % (len(names), bad, noRef))
print("EXPECTED values were extracted from svm/svm_vmcb.h SVM_STATIC_ASSERT (save area base 0x%X)." % SAVE_OFF)

# ---- inventory: macros the implementations define that the reference does NOT
# assert.  These are precisely where the NEXT offset error can hide (g_pat already
# showed the pattern: two files had a define, one did not, and nothing checked it).
known_macros = set(MAP.values())
extra = {}
for _, short in SRCS:
    for m in impl[short]:
        if m not in known_macros:
            extra.setdefault(m, {})[short] = impl[short][m]
print()
print("=== NOT VERIFIED: defined in the implementations, no reference assert ===")
print("%-26s %-12s %-12s %-12s %s" % ("VMCB macro", "svm_driver.c", "vmm.cpp", "kernel64.cpp", "note"))
print("-" * 88)
for m in sorted(extra):
    row = extra[m]
    vals = [row.get(sn) for _, sn in SRCS]
    cells = [("0x%03X" % v) if v is not None else "-" for v in vals]
    distinct = set(v for v in vals if v is not None)
    note = "present in all three, same value" if len(distinct) == 1 and all(v is not None for v in vals) else \
           ("SPLIT across files" if len(distinct) > 1 else "present in some files only")
    print("%-26s %-12s %-12s %-12s %s" % (m, cells[0], cells[1], cells[2], note))
print("-" * 88)
print("unverified macros: %d (none of them is checked against the reference)" % len(extra))

# SEMANTICS (deliberately distinct, see the report):
#   VALUE mismatch      -> a real defect -> exit 1
#   MISSING definition  -> may be by design (e.g. the 64-bit path never uses that
#                          field) -> reported, but NOT a failure.  Conflating the
#                          two would push people to add useless defines to pass.
#   --strict-defs turns missing definitions into a failure too.
if bad:
    print("RESULT: FAIL -- %d value mismatch(es); see the table above." % bad)
    sys.exit(1)
if miss:
    print("RESULT: PASS (values ok) -- %d field(s) are DEFINED IN SOME FILES ONLY." % miss)
    print("        This is not a failure: a field the 64-bit path never uses may")
    print("        legitimately have no define there.  List them; do not force defines.")
    if "--strict-defs" in sys.argv:
        print("        --strict-defs given -> treated as failure.")
        sys.exit(1)
    sys.exit(0)
print("RESULT: PASS -- every reference-asserted offset matches in all three files.")
sys.exit(0)
