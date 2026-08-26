#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Refine the AI-redrawn NexOS app icons: higher resolution + finer,
anti-aliased rounded corners.

The ImageGen backend emits no alpha, so transparency is recovered by
flood-filling the light background (seeded from the 4 corners).  The first
pass (`ai_redraw_postprocess.py`) did this at 128px with a single
`GaussianBlur(1)` and then tex_pack downscaled 128 -> 64 -> (GUI) 24/32,
which left the rounded corners looking chunky/aliased on the desktop.

This pass instead:
  * works on the FULL-RES AI source (~1024px from assets/_gen, or the
    current 128px asset if the gen file is missing),
  * flood-fills the background at a high working resolution (WORK_RES) so
    the tile silhouette is captured crisply,
  * downsamples the recovered alpha with LANCZOS.  Because the binary mask
    is generated at WORK_RES and shrunk to OUT_SIZE, every rounded edge gets
    genuine sub-pixel anti-aliasing -- the corners become smooth/delicate
    instead of 1px-stair-stepped,
  * outputs OUT_SIZE x OUT_SIZE RGBA (default 256, double the old 128),
    cover-fitted with a small inset so the tile fills the frame.

Run:  python3 tools/ai_icon_refine.py
Then: python3 tools/tex_pack.py   (bump TEX icon size to 128 in tex_pack.py)
      tools/build_win.sh build/os.img
"""
import os
import math
from collections import deque
from PIL import Image, ImageFilter, ImageOps

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GEN = os.path.join(ROOT, "assets", "_gen")
ASSETS = os.path.join(ROOT, "assets")

OUT_SIZE = 256      # final icon resolution (was 128)
WORK_RES = 768      # flood-fill resolution; >OUT_SIZE => sub-pixel AA on shrink
INSET = 0.04        # margin left around the tile when cover-fitting
T = 110             # flood-fill tolerance vs. corner-seeded background

# (target filename, generated filename) -- same sources as ai_redraw_postprocess.py
ICONS = [
    ("icon_k0.png", "Flat_modern_app_icon__a_solid__2026-08-21T05-45-40.png"),
    ("icon_k1.png", "Flat_modern_app_icon__a_solid__2026-08-21T05-46-17.png"),
    ("icon_k2.png", "Flat_modern_app_icon__a_solid__2026-08-21T05-46-45.png"),
    ("icon_k3.png", "Flat_modern_app_icon__a_solid__2026-08-21T05-47-14.png"),
    ("icon_k4.png", "Flat_modern_app_icon__a_solid__2026-08-21T05-47-43.png"),
    ("icon_k5.png", "Flat_modern_app_icon__a_solid__2026-08-21T05-48-12.png"),
    ("icon_k6.png", "Flat_modern_app_icon__a_solid__2026-08-21T05-48-41.png"),
    ("icon_k7.png", "Flat_modern_app_icon__a_solid__2026-08-21T05-49-09.png"),
    ("icon_k8.png", "Flat_modern_app_icon__a_solid__2026-08-21T05-49-37.png"),
]


def flood_bg_mask(img, tol=110):
    """4-connected flood fill of the light background from the 4 corners.
    Returns a set of background (x,y) coords.  `img` is an RGB image."""
    w, h = img.size
    px = img.load()
    seeds = [(0, 0), (w - 1, 0), (0, h - 1), (w - 1, h - 1)]
    bg = (sum(px[x, y][c] for x, y in seeds) // 4 for c in range(3))
    bg = tuple(bg)
    visited = set(seeds)
    dq = deque(seeds)
    while dq:
        x, y = dq.popleft()
        for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            nx, ny = x + dx, y + dy
            if 0 <= nx < w and 0 <= ny < h and (nx, ny) not in visited:
                r, g, b = px[nx, ny]
                if max(abs(r - bg[0]), abs(g - bg[1]), abs(b - bg[2])) < tol:
                    visited.add((nx, ny))
                    dq.append((nx, ny))
    return visited, w, h


def refine_icon(src_rgb):
    """Build a high-res, anti-aliased RGBA icon from an AI RGB source."""
    work = src_rgb.resize((WORK_RES, WORK_RES), Image.LANCZOS)
    bg_set, w, h = flood_bg_mask(work, T)
    a = Image.new("L", (w, h), 0)
    apx = a.load()
    for (x, y) in bg_set:
        apx[x, y] = 255
    a = ImageOps.invert(a)                       # bg=0, tile=255
    bbox = a.getbbox()                            # non-zero (tile) region
    if bbox is None:
        bbox = (0, 0, w, h)
    pad = int(INSET * w)
    l = max(0, bbox[0] - pad); t = max(0, bbox[1] - pad)
    r = min(w, bbox[2] + pad); b = min(h, bbox[3] + pad)
    art = work.crop((l, t, r, b)).resize((OUT_SIZE, OUT_SIZE), Image.LANCZOS)
    amask = a.crop((l, t, r, b)).resize((OUT_SIZE, OUT_SIZE), Image.LANCZOS)
    out = Image.new("RGBA", (OUT_SIZE, OUT_SIZE))
    out.paste(art, (0, 0))
    out.putalpha(amask)
    return out


def dewtermark(rgba):
    """Strip the faint AI-platform watermark burned into the bottom-right
    corner of the generated tile.

    The icon tile is a near-flat solid colour (TL<->BL delta <= ~7), so the
    watermark is merely a faint tint over it.  Any corner pixel whose colour
    is close to (but not exactly) the tile background -- and far weaker than
    a real glyph stroke -- is just repainted with the background colour.
    This is seamless because the underlying tile is flat.
    """
    W, H = rgba.size
    px = rgba.load()
    a = rgba.split()[3]
    # background from a clean top-left patch (away from glyph centre + corner mark)
    ps = [rgba.getpixel((x, y))[:3]
          for y in range(int(0.08 * H), int(0.35 * H))
          for x in range(int(0.08 * W), int(0.35 * W))
          if a.getpixel((x, y)) > 200]
    if not ps:
        return rgba
    bg = tuple(sorted(c)[len(c) // 2] for c in zip(*ps))
    out = rgba.copy()
    op = out.load()
    for y in range(int(0.5 * H), H):
        for x in range(int(0.5 * W), W):
            if a.getpixel((x, y)) <= 200:
                continue
            c = op[x, y][:3]
            d = math.dist(c, bg)
            # watermark is faint (1..65); glyph strokes are >100; flat tile ~<=7
            if 1 <= d <= 65:
                op[x, y] = (bg[0], bg[1], bg[2], op[x, y][3])
    return out


def main():
    for tgt, gen in ICONS:
        sp = os.path.join(GEN, gen)
        if not os.path.exists(sp):
            # fall back to the asset already on disk (current res)
            sp = os.path.join(ASSETS, tgt)
        if not os.path.exists(sp):
            print("MISSING", tgt, "(no gen and no asset)")
            continue
        im = Image.open(sp).convert("RGB")
        out = refine_icon(im)
        out = dewtermark(out)
        out.save(os.path.join(ASSETS, tgt), "PNG")
        a = out.split()[3]
        opaque = sum(1 for p in a.getdata() if p > 200)
        print("icon %-12s -> %dx%d RGBA  opaque=%.2f" %
              (tgt, out.size[0], out.size[1], opaque / (OUT_SIZE * OUT_SIZE)))
    print("done.")


if __name__ == "__main__":
    main()
