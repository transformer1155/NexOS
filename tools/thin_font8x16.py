#!/usr/bin/env python3
"""Thin the BIOS font8x16 bitmap (strokes too bold -> slimmer).
Operates directly on gui.cpp's font8x16[256][16] array. Makes a backup first.
For every horizontal run of ink pixels >=3 wide, keep only the two END pixels
(drop the interior), so a 6px-thick stroke becomes 2px. Runs of 1-2 stay.
Vertical strokes in an 8px cell are mostly 1-2px and are left alone.
NOTE: do NOT emit PROGMEM - the i686-elf kernel toolchain has no such macro.
"""
import re, shutil, sys

GUI = "gui.cpp"
BAK = "gui.cpp.font8x16.bak2"

def thin_row(byte):
    bits = [(byte >> (7 - c)) & 1 for c in range(8)]
    runs = []
    i = 0
    while i < 8:
        if bits[i] == 1:
            j = i
            while j < 8 and bits[j] == 1:
                j += 1
            runs.append((i, j))  # [start, end)
            i = j
        else:
            i += 1
    for (s, e) in runs:
        length = e - s
        if length >= 3:
            # keep only the two end pixels; clear the interior
            for p in range(s + 1, e - 1):
                bits[p] = 0
    out = 0
    for c in range(8):
        out = (out << 1) | bits[c]
    return out

def main():
    shutil.copy(GUI, BAK)
    src = open(GUI, encoding="utf-8-sig", errors="replace").read()
    m = re.search(r"const uint8_t font8x16\[256\]\[16\][^=]*= \{(.*?)\};", src, re.S)
    if not m:
        print("font8x16 array not found!"); sys.exit(1)
    nums = [int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", m.group(1))]
    assert len(nums) == 256 * 16, f"expected 4096 bytes, got {len(nums)}"
    before_ink = sum(1 for v in nums if v)
    out = []
    for cp in range(256):
        glyph = nums[cp*16:cp*16+16]
        out.append([thin_row(b) for b in glyph])
    after_ink = sum(1 for g in out for v in g if v)
    lines = []
    for cp in range(256):
        rowstr = ", ".join(f"0x{b:02X}" for b in out[cp])
        lines.append(f"    /* 0x{cp:02X} */ {{{rowstr}}},")
    new_array = "\n".join(lines)
    # NO PROGMEM: i686-elf kernel toolchain lacks it
    new_block = f"const uint8_t font8x16[256][16] = {{\n{new_array}\n}};"
    new_src = src[:m.start()] + new_block + src[m.end():]
    open(GUI, "w", encoding="utf-8").write(new_src)
    print(f"ink before={before_ink} after={after_ink} (thinner: {before_ink-after_ink} fewer pixels)")

if __name__ == "__main__":
    main()
