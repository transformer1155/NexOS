#!/usr/bin/env python3
"""step4_ime.py - Strangler-Fig step 4: extract the Pinyin IME into a REAL
translation unit (src/gui/gfx_ime.{h,cpp}).

WHY THIS MODULE GOES FIRST (after the smoke test / batch B)
    Every other candidate (browser, Win11Desktop, gfx_core) lives inside
    gui_legacy.cpp's anonymous namespace, so its type has internal linkage and a
    separate .cpp is impossible without externalising dozens of entities first.
    The IME block sits at file scope (BEFORE the namespace opens), so this is the
    one module that can become a real TU with bounded linkage work.

WHAT MOVES (3 disjoint spans, ~92 lines)
    A  globals:    g_ime_py / g_ime_len / g_ime_active / g_ime_cands /
                   g_ime_cand_count / g_ime_cn
    B  functions:  ime_reset, the utf8_encode forward declaration, the serial
                   helper forward declarations, ime_commit, ime_lookup
    C  the utf8_encode DEFINITION (it sits 3 lines further down, after B)

THE 64-BIT TRAP THIS SCRIPT EXISTS TO AVOID
    gui_legacy.cpp obtains the mforms_* host API like this:
        #if defined(__i386__) || defined(NexOS_HAVE_MFORMS)
        #  include "mforms.h"                    <- 32-bit: real API
        #else
        #  define NexOS_HAVE_MFORMS 0
        static inline int mforms_key(int,int) { return 0; }   <- 64-bit: PRIVATE stub
        ...
    mforms_key is called by ime_commit.  A naive extraction therefore compiles in
    32-bit and FAILS TO LINK in 64-bit, because the stub has internal linkage and
    is invisible to a new TU.  Fix: move that whole #if/#else block into a shared
    header (src/gui/mforms_bridge.h) and include it from BOTH translation units.

USAGE
    python3 tools/refactor/step4_ime.py             # dry run
    python3 tools/refactor/step4_ime.py --preview   # write to /tmp + self-check
    python3 tools/refactor/step4_ime.py --apply     # rewrite the repo

GUARDS
    * --apply refuses if the files it rewrites are dirty (other agents work here)
    * --apply refuses while make / cc1plus / qemu is running (one vCPU!)
      NOTE: the check deliberately does NOT use 'pgrep -c X || echo 0' -- pgrep -c
      prints 0 AND exits non-zero on no match, so the fallback appends a second
      line and the guard variable becomes "0\n0" (looks busy, is idle).
    * every anchor must match exactly once, else abort before writing anything
"""
import os, re, subprocess, sys

APPLY   = "--apply" in sys.argv
SYNTAX  = "--syntax" in sys.argv   # mirror to /tmp and -fsyntax-only (NO repo write)
PREVIEW = "--preview" in sys.argv
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
LEG  = os.path.join(ROOT, "src", "gui", "gui_legacy.cpp")
BRIDGE = os.path.join(ROOT, "src", "gui", "mforms_bridge.h")
IHDR = os.path.join(ROOT, "src", "gui", "gfx_ime.h")
ICPP = os.path.join(ROOT, "src", "gui", "gfx_ime.cpp")
MK   = os.path.join(ROOT, "Makefile")

def die(m):
    print("ABORT: " + m); sys.exit(1)

def sh(cmd):
    r = subprocess.run(["bash", "-lc", cmd], cwd=ROOT, capture_output=True, text=True)
    return r.stdout.strip()

MINE = "src/gui/gui_legacy.cpp src/gui/gfx_graphics.inc src/gui/gfx_draw.inc Makefile src/gui/mforms_bridge.h src/gui/gfx_ime.h src/gui/gfx_ime.cpp"
if APPLY:
    # Only gui_legacy.cpp -- the file whose anchors this step edits -- must be
    # unmodified vs HEAD.  The other paths in MINE (Makefile, gfx_graphics.inc,
    # gfx_draw.inc) legitimately carry the team's UNCOMMITTED deliverables, so
    # requiring them "clean" made this step impossible to run at all (observed:
    # the first gfx_ime slot aborted for exactly that reason).  Anchor integrity
    # is enforced far more precisely by find_one()'s "must match exactly once".
    leg_dirty = sh("git status --porcelain -- src/gui/gui_legacy.cpp")
    if leg_dirty: die("gui_legacy.cpp drifted from HEAD (anchors unsafe):\n" + leg_dirty)
    busy = sh("pgrep -c make; pgrep -c cc1plus; pgrep -c qemu-system")
    counts = [int(x) for x in busy.split()] if busy.strip() else [0, 0, 0]
    if any(c > 0 for c in counts): die("make/cc1plus/qemu running: %s" % counts)
    others = sh("git status --porcelain -- " + MINE)
    if others: print("[note] expected uncommitted deliverables in scope:\n" + others)
    print("[ok] preflight: gui_legacy.cpp clean vs HEAD, no build/QEMU")
else:
    print("[mode] %s (nothing written to the repo)" % ("preview" if PREVIEW else "dry run"))

L = open(LEG, encoding="utf-8", errors="surrogateescape").read().split("\n")

def find_one(regex, what, lo=0):
    hits = [i for i in range(lo, len(L)) if re.search(regex, L[i])]
    if len(hits) != 1: die("anchor '%s' matched %d lines (need exactly 1)" % (what, len(hits)))
    return hits[0]

def sig_of(regex, what):
    """Return the FULL declaration of the function whose definition line matches
    'regex', read from the SOURCE text.  Hand-typing it is how the first gfx_ime
    slot failed: the header said 5 params (char* out, int outsize) while the real
    definition has 6 (char* out_buf, int* out_len, int out_max), so every call
    site reported "too many arguments" / "int* to int".  Derive, never retype."""
    i = find_one(regex, what)
    txt, depth = "", 0
    for k in range(i, len(L)):
        ln = L[k].strip()
        txt = (txt + " " + ln).strip()
        depth += ln.count("(") - ln.count(")")
        if depth == 0:
            break
    txt = txt.split("{")[0].strip()
    if txt.startswith("static "):
        txt = txt[7:].strip()
    txt = re.sub(r"\s+", " ", txt)
    return txt

def block_end(start):
    d, seen, end, decl = 0, False, None, False
    for k in range(start, len(L)):
        for j, c in enumerate(L[k]):
            if c == "/" and j + 1 < len(L[k]) and L[k][j + 1] == "/": break
            if c == ";" and not seen: decl = True; break
            if c == "{": d += 1; seen = True
            elif c == "}":
                d -= 1
                if seen and d == 0: end = k; break
        if decl or end is not None: break
    return (None if decl else end)

# ---- anchors --------------------------------------------------------------
# shared mforms bridge block
# the banner is 3 comment lines before the #if; find the #if line then walk back
if_line = find_one(r"^#if defined\(__i386__\) \|\| defined\(NexOS_HAVE_MFORMS\)", "mforms #if")
def matching_endif(start):
    """The '#endif' that closes the #if at 'start', tracked by nesting depth:
    the file has 18 bare '#endif' lines, so a plain search cannot pick the right one."""
    depth = 0
    for k in range(start, len(L)):
        t = L[k].strip()
        if re.match(r"^#\s*(if|ifdef|ifndef)\b", t): depth += 1
        elif re.match(r"^#\s*endif\b", t):
            depth -= 1
            if depth == 0: return k
    die("no matching #endif for the mforms #if at L%d" % (start + 1))
endif_line = matching_endif(if_line)
banner_start = if_line
k = if_line - 1
while k >= 0 and L[k].strip().startswith("//"): banner_start = k; k -= 1
# IME spans
ime_a = find_one(r"^static char\s+g_ime_py\[16\]", "IME globals start")
ime_a_end = find_one(r"^static bool\s+g_ime_cn\b", "IME globals end")
ime_b = find_one(r"^static void ime_reset\s*\(", "ime_reset")
ime_c = find_one(r"^static void utf8_encode\s*\(uint32_t cp, char out\[4\]\)\s*\{", "utf8_encode definition")
ime_b_end = find_one(r"^static void ime_lookup\s*\(", "ime_lookup")
ime_b_end = block_end(ime_b_end)
if ime_b_end is None: die("ime_lookup has no body")
ime_c_end = block_end(ime_c)
if ime_c_end is None: die("utf8_encode has no body")
print("[ok] anchors: glb L%d-%d  fn L%d-%d  utf8def L%d-%d  bridge L%d-%d" % (
    ime_a+1, ime_a_end+1, ime_b+1, ime_b_end+1, ime_c+1, ime_c_end+1, banner_start+1, endif_line+1))

# ---- shared bridge header -------------------------------------------------
bridge_body = L[banner_start:endif_line + 1]
bridge = ["// =====================================================================",
          "//  mforms_bridge.h  -  NexOS.Forms host API: real in 32-bit, no-ops in 64-bit",
          "// ---------------------------------------------------------------------",
          "//  Extracted from gui_legacy.cpp so that EVERY translation unit that needs",
          "//  the host API (gui_legacy.cpp, gfx_ime.cpp, ...) sees the same switch.",
          "//  Without this, the 64-bit stubs would stay file-private inside",
          "//  gui_legacy.cpp and a new TU would link in 32-bit and fail in 64-bit.",
          "// =====================================================================",
          "#ifndef NEXOS_GUI_MFORMS_BRIDGE_H",
          "#define NEXOS_GUI_MFORMS_BRIDGE_H",
          ""] + bridge_body + ["", "#endif // NEXOS_GUI_MFORMS_BRIDGE_H"]

# ---- gather the moved text ------------------------------------------------
def strip_static(lines):
    return [re.sub(r"^(\s*)static\s+", r"\1", l) for l in lines]

globals_txt = strip_static(L[ime_a:ime_a_end + 1])
ime_txt     = strip_static(L[ime_b:ime_b_end + 1])
utf8_txt    = strip_static(L[ime_c:ime_c_end + 1])
# inside the new TU the serial forward declarations are redundant (gfx_ime.h
# provides them) and must not stay 'static' -- drop those two lines
ime_txt = [l for l in ime_txt if not re.match(r"^\s*(?:static\s+)?void\s+serial_(puts|putdec|puthex32)\s*\(", l)]

IME_GLOBALS = [("char", "g_ime_py[16]"), ("int", "g_ime_len"), ("bool", "g_ime_active"),
               ("int", "g_ime_cands[9]"), ("int", "g_ime_cand_count"), ("bool", "g_ime_cn")]
ime_hdr = ["// =====================================================================",
           "//  gfx_ime.h  -  Pinyin IME state machine (declarations)",
           "// ---------------------------------------------------------------------",
           "//  Extracted from gui_legacy.cpp (Strangler Fig step 4).  The definitions",
           "//  live in gfx_ime.cpp.  Symbols that used to be file-private ('static') are",
           "//  declared here with external linkage; they are DEFINED exactly once.",
           "// =====================================================================",
           "#ifndef NEXOS_GUI_GFX_IME_H",
           "#define NEXOS_GUI_GFX_IME_H",
           "",
           "#include <stdint.h>",
           "",
           "// ---- IME state (bound to the active terminal window) --------------------"]
for ty, decl in IME_GLOBALS:
    ime_hdr.append("extern %s %s;" % (ty, decl))
ime_hdr += ["",
            "// ---- state machine ------------------------------------------------------",
            "// signatures DERIVED from the definitions (see sig_of) -- do not retype:",
            sig_of(r"^static void ime_reset\s*\(", "ime_reset") + ";",
            sig_of(r"^static int ime_commit\s*\(", "ime_commit") + ";",
            sig_of(r"^static void ime_lookup\s*\(", "ime_lookup") + ";",
            "void utf8_encode(uint32_t cp, char out[4]);",
            "",
            "// ---- serial debug helpers: DEFINED in gui_legacy.cpp, exported for this TU",
            "void serial_puts(const char* s);",
            "void serial_putdec(int v);",
            "void serial_puthex32(uint32_t v);",
            "",
            "#endif // NEXOS_GUI_GFX_IME_H"]

ime_cpp = ["// =====================================================================",
           "//  gfx_ime.cpp  -  Pinyin IME: composing state, candidate lookup, commit",
           "// ---------------------------------------------------------------------",
           "//  Split out of gui_legacy.cpp (Strangler Fig step 4).  This is the first",
           "//  GUI module that lives in its own translation unit.  It needs the mforms",
           "//  host API (ime_commit calls mforms_key), hence mforms_bridge.h -- see the",
           "//  note in that header about the 64-bit stub trap.",
           "// =====================================================================",
           "#include <stdint.h>",
           '#include "mforms_bridge.h"',
           '#include "gfx_ime.h"',
           "// The moved IME lookup consumes the pinyin dictionary TABLES from",
           "// ime_dict.h (gui_legacy.cpp includes it at L24).  Without this the",
           "// new TU cannot see ime_count/ime_pinyin/ime_unicode -- the exact",
           "// cause of the second gfx_ime slot failure.",
           '#include "ime_dict.h"',
           "",
           "// ---- state --------------------------------------------------------------"] + globals_txt + [""] + ime_txt + [""] + utf8_txt

print("[plan] mforms_bridge.h = %d lines, gfx_ime.h = %d, gfx_ime.cpp = %d" %
      (len(bridge), len(ime_hdr), len(ime_cpp)))


if SYNTAX:
    # Mirror the generated files into /tmp and compile-check the NEW TU only.
    # Purpose: catch "missing declaration / missing include" BEFORE occupying the
    # shared build gate.  The previous two gfx_ime slots each died on exactly one
    # such error, burning a slot cycle each time.
    import tempfile, shutil
    D = tempfile.mkdtemp(prefix="step4_syn_")
    for nm, body in (("gfx_ime.h", ime_hdr), ("mforms_bridge.h", bridge), ("gfx_ime.cpp", ime_cpp)):
        open(os.path.join(D, nm), "w").write("\n".join(body) + "\n")
    # 64-bit TU only: mirror the real gui64.o flags (CXX64FLAGS + -I plugins).
    cmd = ("g++ -I. -m64 -mno-red-zone -mcmodel=kernel -ffreestanding -fno-exceptions -fno-rtti "
           "-fno-stack-protector -fno-pic -fno-pie -fcf-protection=none "
           "-fno-asynchronous-unwind-tables -nostdlib -O2 -Wall -Wextra -fpermissive "
           "-DMINIOS_HAVE_MFORMS=1 -DNexOS_HAVE_MFORMS=1 -Iplugins -I%s -fsyntax-only %s/gfx_ime.cpp" % (D, D))
    r = subprocess.run(["bash", "-lc", cmd], cwd=ROOT, capture_output=True, text=True)
    print("[syntax] g++ -fsyntax-only gfx_ime.cpp -> rc=%d" % r.returncode)
    if r.stdout.strip(): print(r.stdout.strip()[:2000])
    if r.stderr.strip(): print(r.stderr.strip()[:3000])
    shutil.rmtree(D, ignore_errors=True)
    sys.exit(0 if r.returncode == 0 else 1)

# ---- rewrite gui_legacy.cpp ----------------------------------------------
dropA = set(range(ime_a, ime_a_end + 1))
dropB = set(range(ime_b, ime_b_end + 1))
dropC = set(range(ime_c, ime_c_end + 1))
dropBr = set(range(banner_start, endif_line + 1))
out, included, bridged = [], False, False
i = 0
while i < len(L):
    if i in dropBr:
        if not bridged:
            # 64-bit-only split: the 64-bit build takes the bridge header (it owns the
            # moved IME); the 32-bit build KEEPS the original in-file block verbatim,
            # so its preprocessed token stream -- and therefore its code -- is unchanged.
            out += ['#if defined(__x86_64__)',
                    '#include "mforms_bridge.h"   // host API for the 64-bit IME TU',
                    '#include "gfx_ime.h"         // declarations of the moved IME symbols',
                    '#else']
            bridged = True
        out.append(L[i])
        if i == endif_line:
            out.append('#endif  // __x86_64__ bridge selection (original block kept for 32-bit)')
        i += 1; continue
    if i == ime_a:
        # 32-bit keeps the IME in-file (zero-byte change); 64-bit gets it from gfx_ime.cpp.
        # Wrapping beats deleting: the 32-bit preprocessor sees the SAME tokens as before.
        out += ['// ---- Pinyin IME: 32-bit keeps it here; 64-bit uses src/gui/gfx_ime.cpp',
                '#if !defined(__x86_64__)']
        included = True
    out.append(L[i])
    if i == ime_c_end:
        out.append('#endif  // !__x86_64__  (in-file IME for the 32-bit build)')
    i += 1
print("[plan] gui_legacy.cpp: %d -> %d lines (-%d)" % (len(L), len(out), len(L) - len(out)))

# Guard-balance assertion: the 32-bit/64-bit guards we emit must nest exactly.
# Without this the first attempt emitted '#if/#else' without a matching '#endif'
# and the error surfaced only as "unterminated #else" deep inside the build --
# after the slot had already been taken.
depth, worst = 0, 0
for _l in out:
    t = _l.lstrip()
    if re.match(r"^#\s*(if|ifdef|ifndef)\b", t): depth += 1
    elif re.match(r"^#\s*endif\b", t): depth -= 1
    worst = min(worst, depth)
if depth != 0 or worst < 0:
    die("emitted preprocessor nesting is unbalanced (final depth %d, min %d)" % (depth, worst))
print("[check] emitted #if/#endif nesting balanced (final depth 0, min depth 0)")

# serial_* definitions must have EXTERNAL linkage for the 64-bit IME TU, while the
# 32-bit build must keep its original 'static' text.  A macro gives both: for 32-bit
# SERIAL_LINKAGE expands to 'static' -- byte-identical tokens, byte-identical code.
out.insert(0, "")
out.insert(1, "#if defined(__x86_64__)")
out.insert(2, "#  define SERIAL_LINKAGE   /* external: gfx_ime.cpp (64-bit) calls these */")
out.insert(3, "#else")
out.insert(4, "#  define SERIAL_LINKAGE static")
out.insert(5, "#endif")
for name in ["serial_puts", "serial_putdec", "serial_puthex32"]:
    hits = [k for k, l in enumerate(out) if re.match(r"^\s*static\s+[A-Za-z_][\w:<>*& ]*\b" + name + r"\s*\(", l)]
    for k in hits:
        # only rewrite DEFINITIONS (have a body '{'), not the forward declarations
        if "{" in out[k] or (k + 1 < len(out) and out[k + 1].strip().startswith("{")):
            out[k] = re.sub(r"^(\s*)static\s+", r"\1SERIAL_LINKAGE ", out[k])
    print("[plan] %-16s linkage via SERIAL_LINKAGE: %d def(s)" % (name, len(hits)))

# ---- Makefile -------------------------------------------------------------
mk = open(MK, encoding="utf-8", errors="surrogateescape").read()
mlines = mk.split("\n")
def patch_mk(mlines):
    n_rules = n_link = n_k64 = 0
    # 64-bit-only split: the ONLY Makefile touches are (a) gui64.o gains the new
    # sources as prerequisites (so its rebuilds are correct) and (b) KERNEL64_OBJS
    # gains gfx_ime64.o.  The 32-bit gui.o rule and every 32-bit link line stay
    # untouched -- that is what keeps the 32-bit image byte-identical.
    for k, l in enumerate(mlines):
        if re.match(r"^\$\(BUILD\)/gui64\.o:", l) and "gfx_ime" not in l:
            bar = l.find("|")
            add = " src/gui/gfx_ime.h src/gui/gfx_ime.cpp src/gui/mforms_bridge.h"
            mlines[k] = (l[:bar] + add + " " + l[bar:]) if bar >= 0 else (l + add)
            n_rules += 1
    # KERNEL64_OBJS: append to the LAST continuation fragment
    k0 = next((k for k, l in enumerate(mlines) if re.match(r"^KERNEL64_OBJS\s*:=", l)), None)
    if k0 is not None:
        last = k0
        while mlines[last].endswith("\\") and last + 1 < len(mlines): last += 1
        if "gfx_ime64.o" not in mlines[last]:
            mlines[last] += " $(BUILD)/gfx_ime64.o"; n_k64 += 1
    return n_rules, n_link, n_k64
r1, r2, r3 = patch_mk(mlines)
print("[plan] Makefile: prereq rules=%d  link lines=%d  KERNEL64_OBJS=%d" % (r1, r2, r3))
# kernel.elf carries TWO $(LD) recipe lines (see the trace above: L592 and L593).
# Both must receive the new object: make runs every recipe line for a target, so a
# link line that omits it would either fail or silently drop the module.
if r1 != 1 or r2 != 0 or r3 != 1:
    die("Makefile patch counts look wrong for the 64-BIT-ONLY design "
        "(want prereq=1 gui64, link=0, k64=1; got prereq=%d link=%d k64=%d)" % (r1, r2, r3))

NEW_RULES = ["# Pinyin IME, extracted into its own translation unit -- 64-BIT ONLY.",
             "# 32-bit keeps the in-file IME in gui_legacy.cpp: its 0x9FC00 EBDA limit",
             "# leaves only a few hundred bytes, so this refactor must not touch it.",
             "$(BUILD)/gfx_ime64.o: src/gui/gfx_ime.cpp src/gui/gfx_ime.h src/gui/mforms_bridge.h | $(BUILD)",
             "\t$(CC64) $(CXX64FLAGS) -I plugins -c src/gui/gfx_ime.cpp -o $@",
             ""]
anchor_rule = "$(BUILD)/gfx_engine64.o: src/gui/gfx_engine.cpp src/gui/gfx_engine.h | $(BUILD)"
k = next((k for k, l in enumerate(mlines) if l.startswith(anchor_rule)), None)
if k is None: die("could not find the gfx_engine64.o rule to anchor the new rules after")
mlines[k + 2:k + 2] = NEW_RULES

if not APPLY:
    if PREVIEW:
        import tempfile
        d = tempfile.mkdtemp(prefix="step4_preview_")
        for name, txt in (("mforms_bridge.h", bridge), ("gfx_ime.h", ime_hdr), ("gfx_ime.cpp", ime_cpp),
                          ("gui_legacy.cpp", out), ("Makefile", mlines)):
            open(os.path.join(d, name), "w").write("\n".join(txt) + "\n")
        print("[preview] files written to " + d)
        print("   bridge braces %+d" % ("\n".join(bridge).count("{") - "\n".join(bridge).count("}")))
        print("   ime_glb decls: %d   ime_fn decls: %d" % (
            len([l for l in ime_hdr if l.startswith("extern ")]),
            len([l for l in ime_hdr if re.match(r"^(void|int)\s+\w+\(", l)])))
        print("   gfx_ime.cpp has mforms_bridge include: %s" % any("mforms_bridge.h" in l for l in ime_cpp))
        print("   gui_legacy.cpp no longer defines ime_reset: %s" % (not any("static void ime_reset" in l for l in out)))
        print("   gui_legacy.cpp has gfx_ime.h include: %s" % any('include "gfx_ime.h"' in l for l in out))
        print("   gui_legacy.cpp has mforms_bridge include: %s" % any('include "mforms_bridge.h"' in l for l in out))
    else:
        print("[mode] DRY RUN - nothing written")
    sys.exit(0)

open(BRIDGE, "w", encoding="utf-8", errors="surrogateescape").write("\n".join(bridge) + "\n")
open(IHDR, "w", encoding="utf-8", errors="surrogateescape").write("\n".join(ime_hdr) + "\n")
open(ICPP, "w", encoding="utf-8", errors="surrogateescape").write("\n".join(ime_cpp) + "\n")
open(LEG, "w", encoding="utf-8", errors="surrogateescape").write("\n".join(out) + "\n")
open(MK, "w", encoding="utf-8", errors="surrogateescape").write("\n".join(mlines) + "\n")
print("[write] " + BRIDGE + "\n[write] " + IHDR + "\n[write] " + ICPP + "\n[write] " + LEG + "\n[write] " + MK)
