#!/usr/bin/env python3
"""Objective portal-desktop check: count near-white card/tile/search-bar
pixels and accent-blue tab/logo pixels in a QEMU PPM screendump.
Since the assistant can't view images, this is the proof that the
managed C# portal surface actually rendered."""
import sys, os
from PIL import Image

def stats(path):
    im = Image.open(path).convert("RGB")
    w, h = im.size
    px = im.load()
    white = accent = 0
    # desktop band: skip the very top (lang indicator) and bottom taskbar
    for y in range(20, h - 60):
        for x in range(0, w):
            r, g, b = px[x, y]
            if r > 230 and g > 230 and b > 230:
                white += 1
            if b > 140 and r < 110 and g > 60 and g < 170 and b > r + 40:
                accent += 1
    return w, h, white, accent

if __name__ == "__main__":
    for f in sys.argv[1:]:
        if not os.path.exists(f):
            print(f"{f}: MISSING")
            continue
        w, h, wh, ac = stats(f)
        print(f"{f}: {w}x{h}  near-white(cards/tiles/searchbar)={wh}  "
              f"accent-blue(tabs/logo/cards)={ac}")
        # Heuristic: a portal desktop has lots of white surfaces and a
        # healthy amount of accent blue.  A plain wallpaper-only or old
        # scattered-icon desktop has far fewer white blocks.
        verdict = "PORTAL-PRESENT" if (wh > 40000 and ac > 200) else "NO-PORTAL"
        print(f"   -> {verdict}")
