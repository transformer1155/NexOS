#!/usr/bin/env python3
"""Offline render-check: replicate gui.cpp draw_char fallback (font16x16,
MSB-first 2 bytes/row) and ASCII-art a few glyphs to prove the font data
produces legible, correctly-scaled 16x16 text."""
import re, sys

def load_font(path):
    s = open(path).read()
    arr = re.search(r'font16x16\[256\]\[32\]\s*=\s*\{(.*?)\};', s, re.S).group(1)
    nums = [int(x, 16) for x in re.findall(r'0x([0-9A-Fa-f]{2})', arr)]
    return nums

def glyph_rows(nums, cp):
    """Return 16 rows of 16 bools, matching draw_char fallback:
    hi = glyph[row*2+0] (pixels 0..7, MSB first), lo = glyph[row*2+1] (8..15)."""
    g = nums[cp*32:cp*32+32]
    out = []
    for r in range(16):
        hi = g[r*2]; lo = g[r*2+1]
        bits = [(hi >> (7-b)) & 1 for b in range(8)] + [(lo >> (7-b)) & 1 for b in range(8)]
        out.append(bits)
    return out

def render(nums, text):
    print(f"--- render: {text!r} ---")
    cps = [ord(c) for c in text]
    # stack rows so glyphs sit side by side
    for r in range(16):
        line = ""
        for cp in cps:
            rows = glyph_rows(nums, cp)
            line += "".join("#" if v else "." for v in rows[r]) + " "
        print(line)
    print()

def main():
    nums = load_font("font16x16.h")
    assert len(nums) >= 256*32, f"expected >=8192 bytes, got {len(nums)}"
    # Check 0x20..0x7F has real ink (proves generator worked, not all-zero)
    ink = sum(1 for cp in range(0x20, 0x80) if any(glyph_rows(nums, cp)[r][c] for r in range(16) for c in range(16)))
    print(f"ASCII glyphs with ink: {ink}/96 (must be >0)")
    assert ink > 0, "FATAL: no ASCII glyphs have ink -> font data is broken"
    render(nums, "AB")
    render(nums, "NexOS")
    render(nums, "Hi! 0x41")

if __name__ == "__main__":
    main()
