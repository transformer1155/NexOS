#!/usr/bin/env python3
"""audit_moved.py - prove moved function bodies are TEXTUALLY IDENTICAL to the
originals, so that "moved" can be told apart from "edited".

Why a second layer: .text comparison proves BEHAVIOUR did not change; it cannot
prove no line was silently edited (a rename or a dropped comment can survive as
identical machine code).  This script answers the other question.

USAGE
    audit_moved.py --rev HEAD --src src/gui/gui_legacy.cpp --dst src/gui/gfx_ime.cpp
                   --names ime_reset,ime_commit,ime_lookup,utf8_encode

SOURCE SIDE: the function is located in <rev>:<src> whatever its indentation
(inside a struct or at file scope) and its body extracted brace-matched.
DEST SIDE  : the definition in <dst> is located (optionally qualified with
'Class::') and its body extracted the same way.
Only bodies are compared, normalised by stripping trailing whitespace and blank
edges -- signatures legitimately change ('static' dropped, 'Graphics::' added).

MATCHER NOTES (three shapes were each discovered the hard way):
    * skip lines whose lstrip is '//'  -- a comment mentioning 'f(' is not a def
    * allow '{' not at end of line   -- single-line members exist
    * allow ')' not on the same line -- multi-line signatures exist
"""
import argparse, re, subprocess, sys

ap = argparse.ArgumentParser()
ap.add_argument("--rev", default="HEAD")
ap.add_argument("--src", required=True)
ap.add_argument("--dst", required=True)
ap.add_argument("--names", required=True, help="comma separated function names")
A = ap.parse_args()
NAMES = [n for n in A.names.split(",") if n]

def load_text(rev, path):
    r = subprocess.run(["git", "show", "%s:%s" % (rev, path)], capture_output=True, text=True)
    if r.returncode != 0: print("cannot read %s:%s" % (rev, path)); sys.exit(2)
    return r.stdout.split("\n")

def scan(lines, i):
    d, seen, bl, bc = 0, False, None, None
    state = None
    for k in range(i, len(lines)):
        line = lines[k]; j = 0
        while j < len(line):
            c = line[j]
            if state is None:
                if c == "/" and j+1 < len(line) and line[j+1] == "/": break
                if c == "/" and j+1 < len(line) and line[j+1] == "*": state = "/*"; j += 2; continue
                if c == '"' or c == "'": state = c; j += 1; continue
                if c == ";" and not seen: return None
                if c == "{":
                    if bl is None: bl, bc = k, j
                    d += 1; seen = True
                elif c == "}":
                    d -= 1
                    if seen and d == 0: return (bl, bc, k, j)
            else:
                if state == "/*":
                    if c == "*" and j+1 < len(line) and line[j+1] == "/": state = None; j += 2; continue
                else:
                    if c == "\\": j += 2; continue
                    if c == state: state = None
            j += 1
    return None

def body(lines, span):
    bl, bc, el, ec = span
    if bl == el: return lines[bl][bc+1:ec]
    out = [lines[bl][bc+1:]] + lines[bl+1:el] + [lines[el][:ec]]
    return "\n".join(out)

def norm(t):
    return "\n".join(x.rstrip() for x in t.split("\n")).strip("\n").strip()

def find(lines, name, qualified):
    pat = re.compile(r"^\s*(?:static\s+|inline\s+)?(?:const\s+)?[A-Za-z_]\w*"
                     r"[\w:<>*&\s]*?\b(?:\w+::)?" + name + r"\s*\(")
    for i, l in enumerate(lines):
        if l.lstrip().startswith("//"): continue
        if qualified and ("::" + name) not in l: continue
        if pat.match(l):
            s = scan(lines, i)
            if s: return (i, s)
    return None

src_lines = load_text(A.rev, A.src)
dst_lines = open(A.dst, encoding="utf-8", errors="surrogateescape").read().split("\n")

ok = True
print("%-26s %-9s %-9s %s" % ("function", "src@rev", "dst", "verdict"))
for name in NAMES:
    s = find(src_lines, name, False)
    d = find(dst_lines, name, True)
    if not s or not d:
        print("%-26s %-9s %-9s MISSING (src=%s dst=%s)" % (name, bool(s), bool(d), bool(s), bool(d)))
        ok = False; continue
    same = norm(body(src_lines, s[1])) == norm(body(dst_lines, d[1]))
    if not same: ok = False
    print("%-26s L%-8d L%-8d %s" % (name, s[0]+1, d[0]+1, "IDENTICAL" if same else "DIFFERS"))
    if not same:
        import difflib
        a = norm(body(src_lines, s[1])).split("\n"); b = norm(body(dst_lines, d[1])).split("\n")
        for l in list(difflib.unified_diff(a, b, lineterm=""))[:12]: print("      " + l)
print()
print("RESULT: " + ("PASS - bodies verbatim" if ok else "FAIL - see above"))
sys.exit(0 if ok else 1)
