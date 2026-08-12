#!/usr/bin/env python3
"""Quick histogram probe for the freshly downloaded menu/taskbar textures."""
import os
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

for name in ("menu.png", "taskbar.png"):
    p = os.path.join(ROOT, "assets", name)
    im = Image.open(p).convert("RGB")
    w, h = im.size
    px = im.load()
    import collections
    hist = collections.Counter()
    for yy in range(0, h, 3):
        for xx in range(0, w, 3):
            r, g, b = px[xx, yy]
            hist[(r // 64, g // 64, b // 64)] += 1
    print(name, "%dx%d" % im.size)
    for k, v in hist.most_common(6):
        print("  bucket", k, "count", v)
    # corner sample
    print("  corner(0,0)=", px[0, 0], "center=", px[w // 2, h // 2])
