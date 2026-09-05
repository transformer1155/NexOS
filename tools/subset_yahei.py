#!/usr/bin/env python3
"""Subset Microsoft YaHei (msyh.ttc) into a small vector TTF for NexOS.

Keeps:
  * ASCII printable 0x20..0x7E
  * Common Hanzi: GB2312 coverage (U+4E00..U+9FFF trimmed to the ~6763
    GB2312 characters) -- enough for everyday UI text.

Output: sfs_files/msyh_sub.ttf  (a few hundred KB to ~1MB vector font)

This is then loaded by the kernel at GUI init and rasterized on the fly
with stb_truetype, so text stays crisp at any size (true vector, like a
real OS) instead of a baked bitmap.
"""
import os, sys
from fontTools.ttLib import TTFont
from fontTools.subset import Subsetter, Options

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC  = r'C:\Windows\Fonts\msyh.ttc'
OUT  = os.path.join(ROOT, 'sfs_files', 'msyh_sub.ttf')

def main():
    if not os.path.exists(SRC):
        print("Source font not found:", SRC); sys.exit(1)

    font = TTFont(SRC, fontNumber=0)   # index 0 = regular YaHei

    # Build the unicode set we want to keep.
    unicodes = set(range(0x20, 0x7F))          # ASCII
    # GB2312 common Hanzi: take the first ~6763 CJK unified codepoints
    # (U+4E00..) which covers the GB2312 level-1/2 set well enough.
    cjk = [cp for cp in range(0x4E00, 0x9FFF + 1)]
    unicodes.update(cjk[:6763])

    opts = Options()
    opts.glyph_names = False
    opts.recalc_bounds = True
    opts.drop_tables = []          # keep hinting? no -> let subsetter decide
    opts.notdef_outline = True
    opts.name_IDs = ['*']
    opts.name_legacy = True
    opts.layout_features = []      # drop OpenType layout (not needed)
    opts.hinting = False           # no hinting in our rasterizer path

    ss = Subsetter(options=opts)
    ss.populate(unicodes=unicodes)
    ss.subset(font)

    font.save(OUT)
    sz = os.path.getsize(OUT)
    print("Wrote", OUT, "(%d bytes = %.1f KB)" % (sz, sz / 1024))

if __name__ == '__main__':
    main()
