#!/usr/bin/env python3
# Deterministic verification of the h_file_name space-truncation fix.
#
# Ported 1:1 from tools/test_hfilename.c (which copies mforms.cpp::h_file_name
# verbatim).  No native C compiler exists on this Windows host, so the exact C
# pointer/loop logic is mirrored in Python to keep the proof faithful.
#
# Both the desktop right-click delete and the file-manager delete resolve the
# target name through Host.FileName (mforms.cpp::h_file_name) -> h_file_delete
# -> g_h.remove -> kernel mkfs.remove.  OLD code stopped at the first space, so
# "This PC.lnk" became "This" and mkfs.remove("This") silently no-op'd.  NEW
# code extends to end-of-line (or to the SFS " (NNNB)" size annotation).

def extract_new(line: str):
    # const char* l = line;  (work on a list of chars)
    ch = list(line)
    i = 0
    if ch[0] == '[' and ch[1] == 'D' and ch[2] == ']':
        i += 3
    while i < len(ch) and ch[i] == ' ':
        i += 1
    l = i
    # end -> NUL terminator
    end = len(ch)
    # scan for SFS " (digitsB)" suffix
    p = l
    while p < end:
        if (ch[p] == '(' and p > l and ch[p-1] == ' ' and
                p + 1 < end and '0' <= ch[p+1] <= '9'):
            q = p + 1
            while q < end and '0' <= ch[q] <= '9':
                q += 1
            if q < end and ch[q] == 'B' and q + 1 < end and ch[q+1] == ')':
                end = p
                break
        p += 1
    # trim trailing newline/space
    while end > l and (ch[end-1] == '\n' or ch[end-1] == ' '):
        end -= 1
    return ''.join(ch[l:end])


def extract_old(line: str):
    ch = list(line)
    i = 0
    if ch[0] == '[' and ch[1] == 'D' and ch[2] == ']':
        i += 3
    while i < len(ch) and ch[i] == ' ':
        i += 1
    l = i
    end = l
    while end < len(ch) and ch[end] != ' ':
        end += 1
    return ''.join(ch[l:end])


cases = [
    # desktop shortcuts (MKFS/Desktop = bare name)
    ("This PC.lnk\n",       "This PC.lnk"),
    ("Task Mgr.lnk\n",      "Task Mgr.lnk"),
    ("AI Agent.lnk\n",      "AI Agent.lnk"),
    # file-manager auto-named file (MKFS = bare name, has a space)
    ("New File.txt\n",      "New File.txt"),
    # SFS listing carries a " (NNNB)" size suffix
    ("readme.txt (1234B)\n", "readme.txt"),
    ("my doc (4096B)\n",    "my doc"),
]

print("=== h_file_name verification (NEW = fixed, OLD = buggy) ===")
fails = 0
for raw, expect in cases:
    rnew = extract_new(raw)
    rold = extract_old(raw)
    ok = (rnew == expect)
    if not ok:
        fails += 1
    print(f"[{'PASS' if ok else 'FAIL'}] raw={raw!r:<22} NEW={rnew:<14} OLD={rold:<10} {'' if ok else '(expected NEW to match full name)'}")

print(f"=== {len(cases)-fails}/{len(cases)} passed ===")
raise SystemExit(1 if fails else 0)
