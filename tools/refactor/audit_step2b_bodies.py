#!/usr/bin/env python3
# Audit step 2b-A: prove the 7 moved method bodies are TEXTUALLY IDENTICAL to
# what they were inside struct Graphics at HEAD, i.e. nothing was edited, only
# relocated (plus 'inline' + 'Graphics::' on the signature).
import subprocess, sys, re

TARGETS = ["draw_char","draw_cjk","draw_cjk_transparent","draw_image_clip",
           "text_width","draw_search_bar","blit_cjk_alpha"]

def load(rev, path):
    r = subprocess.run(["git","show","%s:%s" % (rev, path)], capture_output=True, text=True)
    if r.returncode != 0: print("cannot read %s:%s" % (rev, path)); sys.exit(2)
    return r.stdout.split("\n")

def brace_body(lines, start):
    """find first '{' from start, brace-match, return inner lines (exclusive)."""
    bl = bc = None
    for i in range(start, len(lines)):
        j = lines[i].find("{")
        if j >= 0: bl, bc = i, j; break
        if lines[i].strip().endswith(";"): return None
    if bl is None: return None
    d = 0
    for i in range(bl, len(lines)):
        for j, c in enumerate(lines[i]):
            if i == bl and j < bc: continue
            if c == "{": d += 1
            elif c == "}":
                d -= 1
                if d == 0: return lines[bl+1:i]                      # inner body
    return None

head = load("HEAD", "src/gui/gfx_graphics.inc")
draw = load("HEAD" if False else "HEAD", "src/gui/gfx_draw.inc") if False else open("src/gui/gfx_draw.inc", encoding="utf-8", errors="surrogateescape").read().split("\n")

def find_def(lines, name, want_class):
    pat = re.compile(r"^\s*(?:inline\s+)?[A-Za-z_][\w:<>*&\s]*?\b" + name + r"\s*\(")
    for i, l in enumerate(lines):
        if pat.match(l):
            if want_class and ("Graphics::" + name) not in l: continue
            return i
    return -1

def norm(txt):
    return "\n".join(x.rstrip() for x in txt).strip("\n").strip()

ok = True
print("%-24s %-8s %-8s %s" % ("method","HEAD","DRAW","verdict"))
for t in TARGETS:
    i_head = find_def(head, t, False)
    i_draw = find_def(draw, t, True)
    if i_head < 0 or i_draw < 0:
        print("%-24s %-8s %-8s MISSING" % (t, i_head, i_draw)); ok = False; continue
    b_head = brace_body(head, i_head)
    b_draw = brace_body(draw, i_draw)
    if b_head is None or b_draw is None:
        print("%-24s %-8s %-8s UNBALANCED" % (t, i_head, i_draw)); ok = False; continue
    same = norm(b_head) == norm(b_draw)
    if not same: ok = False
    print("%-24s L%-7d L%-7d %s  (%d/%d lines)" % (t, i_head+1, i_draw+1,
          "IDENTICAL" if same else "DIFFERS", len(b_head), len(b_draw)))
    if not same:
        import difflib
        for d in list(difflib.unified_diff(norm(b_head).split("\n"), norm(b_draw).split("\n"), lineterm=""))[:12]:
            print("      " + d)

print()
print("RESULT:", "PASS - all 7 bodies moved verbatim, none edited or lost" if ok else "FAIL")
sys.exit(0 if ok else 1)
