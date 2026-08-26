#!/usr/bin/env python3
"""Rebuild gui.cpp's font8x16 from git HEAD (clean 4096-byte / 256x16 table,
no PROGMEM) and thin it (horizontal runs >=3px keep only the two end pixels).
Leaves all other gui.cpp edits (g_font_w/h/px, comments) intact.
"""
import re, subprocess, sys

GUI = "gui.cpp"

def thin_row(byte):
    bits = [(byte >> (7 - c)) & 1 for c in range(8)]
    runs = []
    i = 0
    while i < 8:
        if bits[i] == 1:
            j = i
            while j < 8 and bits[j] == 1:
                j += 1
            runs.append((i, j))
            i = j
        else:
            i += 1
    for (s, e) in runs:
        length = e - s
        if length >= 3:
            for p in range(s + 1, e - 1):
                bits[p] = 0
    out = 0
    for c in range(8):
        out = (out << 1) | bits[c]
    return out

# 1) pull the ORIGINAL font table from git HEAD
head = subprocess.check_output(["git", "show", "HEAD:gui.cpp"],
                               cwd="d:/MyOS/bootloader").decode("utf-8", "replace")
m = re.search(r"const uint8_t font8x16\[256\]\[16\][^=]*= \{(.*?)\};", head, re.S)
if not m:
    print("HEAD: font8x16 not found"); sys.exit(1)
nums = [int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", m.group(1))]
assert len(nums) == 256 * 16, f"HEAD font8x16 len {len(nums)}"
before = sum(1 for v in nums if v)
out = []
for cp in range(256):
    glyph = nums[cp*16:cp*16+16]
    out.append([thin_row(b) for b in glyph])
after = sum(1 for g in out for v in g if v)

lines = []
for cp in range(256):
    rowstr = ", ".join(f"0x{b:02X}" for b in out[cp])
    lines.append(f"    /* 0x{cp:02X} */ {{{rowstr}}},")
new_array = "\n".join(lines)
# NO PROGMEM (i686-elf kernel toolchain lacks it)
new_block = f"const uint8_t font8x16[256][16] = {{\n{new_array}\n}};"

# 2) replace whatever is currently in gui.cpp (possibly mangled / PROGMEM)
src = open(GUI, encoding="utf-8", errors="replace").read()
pat = re.compile(r"const uint8_t font8x16\[256\]\[16\][^=]*= \{.*?\n\};", re.S)
if not pat.search(src):
    print("current gui.cpp: font8x16 block not found"); sys.exit(1)
src2 = pat.sub(new_block, src, count=1)
open(GUI, "w", encoding="utf-8").write(src2)
print(f"rebuilt font8x16 from HEAD + thinned: ink {before} -> {after} ({-(before-after)} fewer)")
