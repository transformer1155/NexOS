#!/usr/bin/env python3
"""Subset a LIGHTER CJK font into sfs_files/msyh.ttf so the OS GUI renders
thinner text.  Source: Microsoft YaHei Light (msyhl.ttc) on the Windows host
-- the same family as the current font but a lighter weight, which directly
addresses the "font is too thick" complaint.
"""
import os, shutil, codecs
from fontTools.ttLib import TTFont
from fontTools.subset import Subsetter, Options

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC  = "/mnt/c/Windows/Fonts/msyhl.ttc"
DST  = os.path.join(ROOT, "sfs_files", "msyh.ttf")
BACKUP = os.path.join(ROOT, "sfs_files", "msyh_regular_backup.ttf")

if os.path.exists(DST) and not os.path.exists(BACKUP):
    shutil.copy(DST, BACKUP)
    print("backed up current font ->", os.path.basename(BACKUP))

def style_name(f):
    try:
        name = f["name"]
        fam = name.getDebugName(1) or ""
        sub = name.getDebugName(2) or ""
        full = name.getDebugName(4) or ""
        return fam, sub, full
    except Exception:
        return "", "", ""

# Enumerate faces in the .ttc and pick the lightest (style contains "Light").
chosen = None
for i in range(4):
    try:
        f = TTFont(SRC, fontNumber=i)
    except Exception as e:
        break
    fam, sub, full = style_name(f)
    print(f"face {i}: family={fam!r} subfamily={sub!r} full={full!r}")
    low = (sub + " " + full).lower()
    if chosen is None or "light" in low:
        chosen = (i, f, low)
    else:
        f.close()

font_no, f, low = chosen
print(">>> using face", font_no, "(style hint:", low, ")")

# ---- build the character set (same as tools/subset_font.py) ----
chars = set()
for c in range(0x20, 0x7F):
    chars.add(chr(c))
gb = codecs.getincrementaldecoder("gb2312")("replace")
for hi in range(0xB0, 0xD8):          # GB2312 level-1 zones 16..55
    for lo in range(0xA1, 0xFF):
        try:
            s = bytes([hi, lo]).decode("gb2312")
            if s and s != "\ufffd":
                chars.add(s)
        except Exception:
            pass
extra = "　、。，．·：；！？“”‘’（）《》〈〉「」『』【】〔〕—…–—±×÷≈≠≤≥℃°☆★○●△▲□■◇◆●►«»—―“”‘’〈〉「」『』【】〔〕〖〗〘〙〚〛〜〝〞〟〰〽〿…‧﹏" \
        "①②③④⑤⑥⑦⑧⑨⑩⑴⑵⑶⑷⑸⑹⑺⑻⑼⑽✔✘←↑→↓↔↕⇒∈∉⊂⊃∪∩∞π∑√∫∂∇†‡§¶" \
        "０１２３４５６７８９"
for c in extra:
    chars.add(c)

text = "".join(sorted(chars))
print("subset char count:", len(text))

opt = Options()
opt.glyph_names = False
opt.recalc_bounds = True
opt.notdef_outline = True
opt.name_IDs = ["*"]
opt.name_legacy = True
opt.name_languages = ["*"]
opt.hinting = False          # strip TrueType instructions (fpgm/prep/cvt) -- stb_truetype ignores them anyway
opt.drop_tables = ["LTSH", "MERG", "meta", "VDMX", "hdmx", "gasp",
                   "GSUB", "GPOS", "GDEF", "BASE", "JSTF",
                   "morx", "kerx", "vhea", "vmtx", "VORG", "DSIG",
                   "SVG ", "sbix", "COLR", "CPAL", "CBDT", "CBLC", "EBLC", "EBDT", "PCLT"]

ss = Subsetter(options=opt)
ss.populate(text=text)
ss.subset(f)
f.save(DST)
print("wrote", DST, os.path.getsize(DST), "bytes")
