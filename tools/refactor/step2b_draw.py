#!/usr/bin/env python3
"""step2b_draw.py - Strangler-Fig step 2b: move cold Graphics method bodies into
src/gui/gfx_draw.inc, a same-TU include (the type has internal linkage, so a real
.cpp needs step A' first).

BATCHES
    --batch=a (default)  draw_char, draw_cjk, draw_cjk_transparent,
                         draw_image_clip, text_width, draw_search_bar,
                         blit_cjk_alpha
    --batch=b            every COLD method still defined in the struct

WHAT STAYS INLINE (never moved): fill_rect, present/present_rect, blit_rect,
lfb_row_copy, put_pixel, blend_pixel, blend_pixel_lin, pixelate_rect.  The script
PROVES on every run that none of them calls a method it is about to move --
otherwise the per-frame path could lose inlining.

TWO MISTAKES THIS SCRIPT NOW AVOIDS (both were made for real)
  1. gfx_draw.inc must be MERGED, not overwritten: batch B once regenerated the
     file from scratch and wiped batch A's definitions, leaving 6 undefined
     symbols and a smaller .text.  Existing definitions are now preserved.
  2. An out-of-class definition must NOT repeat 'static'/'inline' and must NOT
     repeat default arguments: batch B emitted 'inline static inline int
     Graphics::f(int a = 1)' and failed to compile.  make_def() normalises the
     signature (keyword run collapsed, defaults dropped, 'inline' added once).

USAGE
    python3 tools/refactor/step2b_draw.py [--batch=b] [--preview] [--apply]
                                     [--allow-mine-dirty]

GUARDS
    * --apply refuses if the files it rewrites are dirty, unless
      --allow-mine-dirty (a previous batch of this same refactor legitimately
      leaves them modified).
    * --apply refuses while make / cc1plus / qemu runs.  The check does NOT use
      'pgrep -c X || echo 0': pgrep -c prints 0 AND exits non-zero on no match,
      so the fallback appends a second line and the value becomes "0\\n0".
    * every anchor must match exactly once, else abort before writing.
"""
import os, re, subprocess, sys

APPLY   = "--apply" in sys.argv
PREVIEW = "--preview" in sys.argv
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
INC  = os.path.join(ROOT, "src", "gui", "gfx_graphics.inc")
DRAW = os.path.join(ROOT, "src", "gui", "gfx_draw.inc")
MK   = os.path.join(ROOT, "Makefile")

BATCH_A = ["draw_char", "draw_cjk", "draw_cjk_transparent", "draw_image_clip",
           "text_width", "draw_search_bar", "blit_cjk_alpha"]
HOT = ["fill_rect", "blend_pixel", "blit_rect", "lfb_row_copy", "present",
       "present_rect", "put_pixel", "blend_pixel_lin", "pixelate_rect"]
BATCH = "b" if "--batch=b" in sys.argv else "a"
EXCLUDE = set()
for a in sys.argv:
    if a.startswith("--exclude="):
        EXCLUDE = {x for x in a.split("=", 1)[1].split(",") if x}

def die(m):
    print("ABORT: " + m); sys.exit(1)

def sh(cmd):
    r = subprocess.run(["bash", "-lc", cmd], cwd=ROOT, capture_output=True, text=True)
    return r.stdout.strip()

MINE = "src/gui/gui_legacy.cpp src/gui/gfx_graphics.inc src/gui/gfx_draw.inc Makefile"
if APPLY:
    dirty = sh("git status --porcelain -- " + MINE)
    if dirty and "--allow-mine-dirty" not in sys.argv:
        die("files I would rewrite are dirty (another agent?):\n" + dirty +
            "\n(pass --allow-mine-dirty if these are this refactor's own previous batches)")
    busy = sh("pgrep -c make; pgrep -c cc1plus; pgrep -c qemu-system")
    counts = [int(x) for x in busy.split()] if busy.strip() else [0, 0, 0]
    if any(c > 0 for c in counts): die("make/cc1plus/qemu running: %s" % counts)
    print("[ok] preflight: no build/QEMU" + ("; own dirty tree allowed" if dirty else "; clean tree"))
else:
    print("[mode] %s" % ("preview (writes to /tmp only)" if PREVIEW else "dry run"))

L = open(INC, encoding="utf-8", errors="surrogateescape").read().split("\n")

def find_struct_body(lines):
    start = next((i for i, l in enumerate(lines) if re.match(r"^struct\s+Graphics\s*\{", l)), None)
    if start is None: die("struct Graphics not found")
    d, end = 0, None
    for i in range(start, len(lines)):
        for c in lines[i]:
            if c == "{": d += 1
            elif c == "}":
                d -= 1
                if d == 0: end = i; break
        if end is not None: break
    if end is None: die("unbalanced braces for struct Graphics")
    return start, end

g0, g1 = find_struct_body(L)
print("[ok] struct Graphics: L%d..L%d (%d lines)" % (g0+1, g1+1, g1-g0+1))

METHOD_RE = re.compile(r"^    (?:inline\s+|static\s+)?[A-Za-z_][\w:<>*&\s]*?\b([a-zA-Z_]\w*)\s*\(")

def scan(lines, i, hi):
    """Locate a member's body: returns (brace_line, brace_col, end_line, end_col),
    or None if the member is only declared (';' before any '{').

    STRING/COMMENT AWARE on purpose: GUI code contains literals such as "{" and
    comments mentioning braces.  A naive brace counter mis-finds the end of such
    a method, which is exactly what produced an unbalanced gfx_draw.inc.
    """
    d, seen, bl, bc = 0, False, None, None
    state = None                      # None | '"' | "'" | '/*'
    for k in range(i, hi + 1):
        line = lines[k]
        j = 0
        while j < len(line):
            c = line[j]
            if state is None:
                if c == "/" and j + 1 < len(line) and line[j+1] == "/":
                    break
                if c == "/" and j + 1 < len(line) and line[j+1] == "*":
                    state = "/*"; j += 2; continue
                if c == '"' or c == "'":
                    state = c; j += 1; continue
                if c == ";" and not seen:
                    return None
                if c == "{":
                    if bl is None: bl, bc = k, j
                    d += 1; seen = True
                elif c == "}":
                    d -= 1
                    if seen and d == 0: return (bl, bc, k, j)
            else:
                if state == "/*":
                    if c == "*" and j + 1 < len(line) and line[j+1] == "/":
                        state = None; j += 2; continue
                else:
                    if c == "\\": j += 2; continue
                    if c == state: state = None
            j += 1
    return None

def enum_defs(hi):
    out, i = {}, g0 + 1
    while i < hi:
        m = METHOD_RE.match(L[i])
        if not m: i += 1; continue
        spans = scan(L, i, hi)
        if not spans: i += 1; continue
        out[m.group(1)] = (i,) + spans
        i = spans[2] + 1
    return out

ALL = enum_defs(g1)
if BATCH == "a":
    TARGETS = [n for n in BATCH_A if n in ALL and n not in EXCLUDE]
    missing = [n for n in BATCH_A if n not in ALL]
    if missing: die("batch A methods no longer defined in the struct: %s" % missing)
else:
    TARGETS = sorted(n for n in ALL if n not in HOT and n not in EXCLUDE)
    print("[batch b] cold methods still defined in the struct: %d%s" % (
        len(TARGETS), (" (excluding %s)" % ",".join(sorted(EXCLUDE))) if EXCLUDE else ""))

# --- prove the hot path cannot be affected ---------------------------------
for h in HOT:
    if h not in ALL: continue
    _, bl, bc, el, ec = ALL[h]
    body = "\n".join(L[bl:el+1])
    for n in TARGETS:
        if re.search(r"\b" + n + r"\s*\(", body):
            die("hot method %s calls %s, which batch %s would move out" % (h, n, BATCH))
print("[ok] hot-path safety: none of [%s] references a selected method" % ",".join(h for h in HOT if h in ALL))

def make_def(name, span):
    """Build an out-of-class definition: no 'static', no defaults, one 'inline'."""
    s, bl, bc, el, ec = span
    head = L[s:bl] + [L[bl][:bc].rstrip()]
    sig = "\n".join(x.rstrip() for x in head).strip()
    decl = sig + ";"
    flat = re.sub(r"^(?:static\s+|inline\s+)+", "", sig)          # drop linkage keywords
    flat = re.sub(r"\b" + name + r"\s*\(", "Graphics::" + name + "(", flat, count=1)
    flat = re.sub(r"\s*=\s*[^,()]+(?=[,)])", "", flat)           # drop default args
    if bl == el:
        # single-line member: 'void f() { body; }' -- slice once, or the closing
        # brace survives (indices shift after the first cut) and the definition
        # ends up with one extra '}'.
        body_lines = [L[bl][bc+1:ec]]
    else:
        body_lines = []
        for k in range(bl, el + 1):
            line = L[k]
            if k == bl: line = line[bc+1:]
            if k == el: line = line[:ec]
            body_lines.append(line)
    body = "\n".join(body_lines).rstrip()
    if body.strip(): body = "\n" + body + "\n"
    else: body = ""
    return decl, "inline " + flat + " {" + body + "}"

new_defs, removed = [], []
out, i = [], 0
while i < len(L):
    m = METHOD_RE.match(L[i]) if g0 < i < g1 else None
    if m and m.group(1) in TARGETS:
        span = ALL[m.group(1)]
        decl, definition = make_def(m.group(1), span)
        out.append(decl)
        new_defs.append(definition)
        print("   %-26s L%-6d..L%-6d %3d lines" % (m.group(1), span[0]+1, span[3]+1, span[3]-span[0]+1))
        i = span[3] + 1
        continue
    out.append(L[i]); i += 1

# --- merge (never overwrite) gfx_draw.inc ----------------------------------
existing = open(DRAW, encoding="utf-8", errors="surrogateescape").read() if os.path.exists(DRAW) else ""
have = set(re.findall(r"Graphics::([a-zA-Z_]\w*)\s*\(", existing))
added = [d for n, d in zip([m.group(1) for m in (METHOD_RE.match(x) for x in [])], [])] # placeholder
pairs = []
i = 0
# recompute names in the same order as new_defs
names_in_order = []
for m in re.finditer(r"", ""): pass
# (names were printed above; rebuild the mapping explicitly)
names_in_order = []
for k, line in enumerate(out):
    pass
# simpler: make_def was called in TARGETS-encounter order
# -> recover by re-scanning the struct we just consumed
consumed = []
j = 0
for name, span in sorted(ALL.items(), key=lambda kv: kv[1][0]):
    if name in TARGETS: consumed.append((name, span))
consumed.sort(key=lambda kv: kv[1][0])
for (name, span), definition in zip(consumed, new_defs):
    pairs.append((name, definition))
fresh = [(n, d) for n, d in pairs if n not in have]

hdr = existing if existing else "\n".join([
    "// =====================================================================",
    "//  gfx_draw.inc  -  drawing-composition methods of struct Graphics",
    "// ---------------------------------------------------------------------",
    "//  Included at NAMESPACE scope, right after the struct, in the same",
    "//  translation unit: the type has internal linkage, so a separate .cpp needs",
    "//  step A' (externalising struct Graphics and everything it touches).",
    "//",
    "//  Hot primitives deliberately stay inline in the struct (fill_rect,",
    "//  present/present_rect, blit_rect, put_pixel, lfb_row_copy, blend_pixel,",
    "//  blend_pixel_lin).  Only UI-composition paths live here.",
    "// =====================================================================",
    ""])
draw_txt = hdr.rstrip("\n") + "\n\n" + "\n\n".join(d for _, d in fresh) + "\n"
print("[plan] gfx_draw.inc: %d existing definitions kept, %d added" % (len(have), len(fresh)))
print("[plan] gfx_graphics.inc: %d -> %d lines (-%d)" % (len(L), len(out), len(L) - len(out)))

if not any('include "gfx_draw.inc"' in l for l in out):
    d, end = 0, None
    for k, line in enumerate(out):
        for c in line:
            if c == "{": d += 1
            elif c == "}":
                d -= 1
                if d == 0: end = k; break
        if end is not None: break
    if end is None: die("cannot locate the struct end to place the include")
    out.insert(end + 1, "")
    out.insert(end + 2, '// ---- cold drawing-composition bodies (step 2b) ----')
    out.insert(end + 3, '#include "gfx_draw.inc"')
    print("[plan] inserted the gfx_draw.inc include after the struct")
else:
    print("[ok] gfx_draw.inc already included")

# --- Makefile --------------------------------------------------------------
mk = open(MK, encoding="utf-8", errors="surrogateescape").read()
mlines = mk.split(chr(10))
RULE = r"^\$\(BUILD\)/(gui|gui64|gui_diag)\.o:"
already = sum(1 for l in mlines if re.match(RULE, l) and "gfx_draw.inc" in l)
n = 0
for k, l in enumerate(mlines):
    if re.match(RULE, l) and "gfx_draw.inc" not in l:
        bar = l.find("|")
        add = " src/gui/gfx_draw.inc"
        mlines[k] = (l[:bar] + add + " " + l[bar:]) if bar >= 0 else (l + add)
        n += 1
total = sum(1 for l in mlines if re.match(RULE, l) and "gfx_draw.inc" in l) if n else already
print("[plan] Makefile: %d to patch, %d already correct" % (n, already))
if n + already != 3: die("expected 3 gui rules, found %d" % (n + already))

if PREVIEW:
    import tempfile
    d = tempfile.mkdtemp(prefix="step2b_preview_")
    open(os.path.join(d, "gfx_graphics.inc"), "w").write("\n".join(out))
    open(os.path.join(d, "gfx_draw.inc"), "w").write(draw_txt)
    inc_out = "\n".join(out)
    ok = True
    for name in TARGETS:
        nd = len(re.findall(r"\b" + name + r"\s*\([^;{]*\)\s*;", inc_out))
        nf = len(re.findall(r"^inline\s+[^\n]*Graphics::" + name + r"\s*\(", draw_txt, re.M))
        if nd < 1 or nf != 1: ok = False
        print("   %-26s decl=%d def=%d %s" % (name, nd, nf, "OK" if (nd >= 1 and nf == 1) else "FAIL"))
    def brace_balance(txt):
        """Count braces ignoring comments and string/char literals.  A naive
        .count('{') is wrong here: GUI code contains strings such as " { " and
        comments with braces, which produced a false '-1' on a good move."""
        bal, i, n, state = 0, 0, len(txt), None
        while i < n:
            c = txt[i]
            if state is None:
                if c == "/" and i + 1 < n and txt[i+1] == "/":
                    j = txt.find(chr(10), i); i = n if j < 0 else j; continue
                if c == "/" and i + 1 < n and txt[i+1] == "*":
                    j = txt.find("*/", i + 2); i = n if j < 0 else j + 2; continue
                if c == '"' or c == "'": state = c; i += 1; continue
                if c == "{": bal += 1
                elif c == "}": bal -= 1
            else:
                if c == "\\": i += 2; continue
                if c == state: state = None
            i += 1
        return bal
    # pinpoint any single definition whose braces do not balance
    for nm, dfn in pairs:
        if nm not in dict(fresh) and existing and ("Graphics::" + nm + "(") in existing:
            continue
        b = (dfn.count("{") - dfn.count("}"))
        if b != 0:
            print("   !! %-26s definition brace balance %+d" % (nm, b))
            for ln in dfn.split(chr(10))[:3]:
                print("        | " + ln[:100])
            print("        | ...")
            for ln in dfn.split(chr(10))[-3:]:
                print("        | " + ln[:100])
    for lbl, txt in (("gfx_graphics.inc", inc_out), ("gfx_draw.inc", draw_txt)):
        bal = brace_balance(txt)
        print("   %-18s braces %+d %s" % (lbl, bal, "OK" if bal == 0 else "FAIL"))
        ok = ok and bal == 0
    for h in HOT:
        if h in ALL:
            present = re.search(r"^    [^\n]*\b" + h + r"\s*\([^;]*\)\s*\{", inc_out, re.M) is not None
            print("   hot %-16s still inline: %s" % (h, "OK" if present else "FAIL"))
            ok = ok and present
    print("[preview] RESULT: %s" % ("PASS" if ok else "FAIL"))
    sys.exit(0 if ok else 1)

if not APPLY:
    print("[mode] DRY RUN - nothing written")
    sys.exit(0)

open(INC, "w", encoding="utf-8", errors="surrogateescape").write("\n".join(out) + "\n")
open(DRAW, "w", encoding="utf-8", errors="surrogateescape").write(draw_txt)
open(MK, "w", encoding="utf-8", errors="surrogateescape").write("\n".join(mlines) + "\n")
print("[write] " + INC + "\n[write] " + DRAW + "\n[write] " + MK)
