#!/usr/bin/env python3
"""Subset msyh_sub.ttf -> ASCII + GB2312 level-1 (3755) + common punctuation.
Keeps size small enough to fit in SFS (~1.3MB free)."""
import sys
from fontTools.ttLib import TTFont
from fontTools.subset import Subsetter, Options

SRC = "msyh_sub.ttf"
DST = "sfs_files/msyh.ttf"

# Build the character set: ASCII printable + GB2312 level-1 + common punctuation
chars = set()
# ASCII 0x20..0x7E
for c in range(0x20, 0x7F):
    chars.add(chr(c))
# GB2312 level-1: 0xB0A1 .. 0xD7FA (3755 chars) -> decode as GB2312
# We walk the GB2312 code space for level-1 (16-55 zones, positions 1-94).
import codecs
gb = codecs.getincrementaldecoder("gb2312")("replace")
for hi in range(0xB0, 0xD8):          # 0xB0..0xD7 inclusive (level-1)
    for lo in range(0xA1, 0xFF):      # 0xA1..0xFE
        try:
            s = bytes([hi, lo]).decode("gb2312")
            if s and s != "\ufffd":
                chars.add(s)
        except Exception:
            pass
# Common CJK punctuation / symbols
extra = "　、。，．·：；！？“”‘’（）《》〈〉「」『』【】〔〕—…–—±×÷≈≠≤≥℃°☆★○●△▲□■◇◆●►«»—―“”‘’〈〉「」『』【】〔〕〖〗〘〙〚〛〜〝〞〟〰〽〿…‧﹏" \
        "①②③④⑤⑥⑦⑧⑨⑩⑴⑵⑶⑷⑸⑹⑺⑻⑼⑽✔✘←↑→↓↔↕⇒∈∉⊂⊃∪∩∞π∑√∫∂∇†‡§¶" \
        "０１２３４５６７８９"
for c in extra:
    chars.add(c)

text = "".join(sorted(chars))
print(f"subset char count: {len(text)}")

opt = Options()
opt.glyph_names = False
opt.recalc_bounds = True
opt.drop_tables = []
opt.notdef_outline = True
opt.name_IDs = ["*"]
opt.name_legacy = True
opt.name_languages = ["*"]

f = TTFont(SRC)
ss = Subsetter(options=opt)
ss.populate(text=text)
ss.subset(f)
f.save(DST)

import os
print(f"wrote {DST}: {os.path.getsize(DST)} bytes")
