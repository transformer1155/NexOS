#!/usr/bin/env python3
"""audit_all_moved.py - prove every method body in gfx_draw.inc is TEXTUALLY
IDENTICAL to the same method inside struct Graphics at HEAD (pre-step-2b).

Why this matters now: batch B is byte-identical in 64-bit but shifts 32-bit .text
by 2 bytes, so .text equality alone cannot carry the "nothing was edited" claim.
This audit answers the different question: was any line altered or lost in the move?

Normalisation applied to both sides: strip trailing whitespace, drop blank edges.
The definition header ('inline ... Graphics::name(...)') is NOT compared - only
the body between the outermost braces.
"""
import subprocess, re, sys

def load(rev, path):
    r = subprocess.run(["git", "show", "%s:%s" % (rev, path)], capture_output=True, text=True)
    if r.returncode != 0: print("cannot read %s:%s" % (rev, path)); sys.exit(2)
    return r.stdout.split("\n")

def scan(lines, i, hi):
    d, seen, bl, bc = 0, False, None, None
    state = None
    for k in range(i, hi + 1):
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

def body(lines, i, span):
    bl, bc, el, ec = span
    if bl == el: return lines[bl][bc+1:ec]
    out = [lines[bl][bc+1:]]
    out += lines[bl+1:el]
    out.append(lines[el][:ec])
    return "\n".join(out)

def norm(t):
    return "\n".join(x.rstrip() for x in t.split("\n")).strip("\n").strip()

head = load("HEAD", "src/gui/gfx_graphics.inc")
h0 = next((i for i, l in enumerate(head) if re.match(r"^struct\s+Graphics\s*\{", l)), None)
if h0 is None: print("no struct Graphics at HEAD"); sys.exit(2)
hd, h1 = 0, None
for i in range(h0, len(head)):
    for c in head[i]:
        if c == "{": hd += 1
        elif c == "}":
            hd -= 1
            if hd == 0: h1 = i; break
    if h1 is not None: break

draw = open("src/gui/gfx_draw.inc", encoding="utf-8", errors="surrogateescape").read().split("\n")

# every out-of-class definition in gfx_draw.inc
defs = {}
i = 0
while i < len(draw):
    m = re.match(r"^inline\s+.*\bGraphics::([a-zA-Z_]\w*)\s*\(", draw[i])
    if m:
        span = scan(draw, i, len(draw) - 1)
        if span: defs[m.group(1)] = body(draw, i, span)
        i = (span[2] + 1) if span else i + 1
        continue
    i += 1

ok, missing, mismatch = True, [], []
for name, dbody in sorted(defs.items()):
    hit = None
    # NOTE: matching must require a REAL definition line.  A loose pattern such
    # as '[^;{]*\bname\s*\(' also matches comment lines like
    # '// rounded via cov_dil1(), which reads ...' -- the scanner then starts at
    # the comment and swallows the following comment block as part of the body,
    # producing a bogus "body differs" verdict (this happened once).
    # THREE shapes must all match, and this is the third iteration on this
    # matcher -- each one exposed a real shape the previous form missed:
    #   1) a comment mentioning 'name('    -> must skip lines whose lstrip is '//'
    #   2) a single-line member           -> '{' need not be at end of line
    #   3) a MULTI-LINE signature         -> the '(' can be the last token on the
    #      line (parameters continue below), so do not require ')' on it either.
    # What is required: 4-space indent, a leading type token, the name, '('.
    defre = re.compile(r"^    (?:inline\s+|static\s+)?(?:const\s+)?[A-Za-z_]\w*"
                       r"[\w:<>*&\s]*?\b" + name + r"\s*\(")
    for k in range(h0 + 1, h1):
        if head[k].lstrip().startswith("//"): continue
        if defre.match(head[k]):
            span = scan(head, k, h1)
            if span: hit = (k, span); break
    if not hit: missing.append(name); ok = False; continue
    hbody = body(head, hit[0], hit[1])
    if norm(hbody) != norm(dbody):
        mismatch.append(name); ok = False

print("definitions found in gfx_draw.inc : %d" % len(defs))
print("matched & identical in HEAD      : %d" % (len(defs) - len(missing) - len(mismatch)))
if missing:  print("NOT FOUND in HEAD                : %s" % ", ".join(missing))
if mismatch: print("BODY DIFFERS from HEAD           : %s" % ", ".join(mismatch))
print()
print("RESULT: " + ("PASS - every moved body is verbatim; the move edited nothing"
                   if ok else "FAIL - see above"))
sys.exit(0 if ok else 1)
