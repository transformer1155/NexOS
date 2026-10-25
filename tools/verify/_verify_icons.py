#!/usr/bin/env python3
# Verify rounded-icon metrics and render real-size + magnified previews.
import os, glob
from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASSETS = os.path.join(ROOT, "assets")
BAK = os.path.join(ASSETS, "_bak_icons_round")
OUT = os.path.join(ROOT, "tools", "_icon_preview")
os.makedirs(OUT, exist_ok=True)

def metrics(path):
    im = Image.open(path).convert("RGBA")
    a = im.split()[3]
    w, h = im.size
    px = a.load()
    bbox = a.getbbox()
    if bbox is None:
        return None
    l, t, r, b = bbox
    margin = min(l, t, w - r, h - b)
    # roundness = transparent fraction inside the top-left R x R corner square.
    # R = approx corner radius guess = margin? no -> use min(tile w, h)/6 capped.
    R = max(6, min(w, h) // 6)
    R = min(R, (r - l) // 3, (b - t) // 3)
    transp = 0
    tot = 0
    for yy in range(t, min(t + R, b)):
        for xx in range(l, min(l + R, r)):
            tot += 1
            if px[xx, yy] < 128:
                transp += 1
    corner_transp = round(100.0 * transp / tot, 1) if tot else 0.0
    # AA quality: count partial-alpha edge pixels (smooth band)
    partial = sum(1 for p in a.getdata() if 8 <= p <= 247)
    opaque = sum(1 for p in a.getdata() if p > 200)
    return dict(margin=int(margin), corner_transp=corner_transp,
                partial=partial, opaque_pct=round(100 * opaque / (w * h), 1))

print("icon            NEW margin  corner%%  partialAA | OLD margin corner%%")
print("-" * 72)
for f in sorted(glob.glob(os.path.join(ASSETS, "icon_k*.png"))):
    name = os.path.basename(f)
    m = metrics(f)
    om = metrics(os.path.join(BAK, name)) if os.path.exists(os.path.join(BAK, name)) else None
    ostr = ("%2d   %5.1f" % (om["margin"], om["corner_transp"])) if om else " n/a"
    print("%-15s  %2d     %5.1f    %5d    | %s" % (name, m["margin"], m["corner_transp"], m["partial"], ostr))

# ---- Previews ----
ICONS = sorted(glob.glob(os.path.join(ASSETS, "icon_k*.png")))
bg_dark = (24, 34, 48)
bg_light = (228, 232, 238)

def make_sheet(bg, fname, scale=1, labels=False):
    n = len(ICONS)
    cols = 3
    rows_n = (n + cols - 1) // cols
    cell = int(32 * scale)
    pad = max(8, 18 * scale)
    W = cols * cell + (cols + 1) * pad
    H = rows_n * cell + (rows_n + 1) * pad + (14 * scale if labels else 0)
    sheet = Image.new("RGB", (W, H), bg)
    d = ImageDraw.Draw(sheet)
    for i, ic in enumerate(ICONS):
        im = Image.open(ic).convert("RGBA").resize((cell, cell), Image.LANCZOS)
        cx = pad + (i % cols) * (cell + pad)
        cy = pad + (i // cols) * (cell + pad)
        sheet.paste(im, (cx, cy), im)
        if labels:
            d.text((cx, cy + cell + 2), "k%d" % i, fill=(150, 160, 175))
    p = os.path.join(OUT, fname)
    sheet.save(p)
    return p

p_dark = make_sheet(bg_dark, "preview_real_dark.png", scale=1)
p_light = make_sheet(bg_light, "preview_real_light.png", scale=1)
p_big = make_sheet(bg_dark, "preview_4x.png", scale=4, labels=True)
print("\nwrote:", p_dark)
print("wrote:", p_light)
print("wrote:", p_big)
