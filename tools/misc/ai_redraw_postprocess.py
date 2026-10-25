#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Post-process AI-generated NexOS UI assets.

- Decorative textures: resize generated RGB to the exact original dimensions.
- Icons (icon_k0..k8): the ImageGen backend does NOT emit an alpha channel, so
  we recover transparency by flood-filling the connected light background that
  surrounds the centred tile (seeded from the 4 corners). The tile + enclosed
  white glyph are never reached by the flood, so they stay opaque. The resulting
  binary mask is lightly blurred for anti-aliased edges and used as the alpha.
"""
import os
from collections import deque
from PIL import Image, ImageFilter, ImageOps, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GEN = os.path.join(ROOT, "assets", "_gen")
ASSETS = os.path.join(ROOT, "assets")

# (target filename, generated filename, (w, h), is_icon)
TASKS = [
    # textures (RGB, just resize)
    ("wallpaper.png", "Windows_11_Bloom_style_desktop_2026-08-21T05-42-33.png", (1920, 1293), False),
    ("Logo.png",      "NexOS_operating_system_logo_wo_2026-08-21T05-43-05.png", (1408, 704),  False),
    ("taskbar.png",   "Windows_11_style_dark_taskbar__2026-08-21T05-43-36.png", (512, 512),   False),
    ("menu.png",      "Windows_11_Start_menu_and_cont_2026-08-21T05-44-04.png", (512, 512),   False),
    ("chrome.png",    "Windows_11_window_title_bar_ch_2026-08-21T05-44-33.png", (400, 600),   False),
    ("winbg.png",     "Windows_11_window_client_area__2026-08-21T05-45-02.png", (400, 267),   False),
    # icons (RGBA, recover alpha via flood fill)
    ("icon_k0.png", "Flat_modern_app_icon__a_solid__2026-08-21T05-45-40.png", (128, 128), True),
    ("icon_k1.png", "Flat_modern_app_icon__a_solid__2026-08-21T05-46-17.png", (128, 128), True),
    ("icon_k2.png", "Flat_modern_app_icon__a_solid__2026-08-21T05-46-45.png", (128, 128), True),
    ("icon_k3.png", "Flat_modern_app_icon__a_solid__2026-08-21T05-47-14.png", (128, 128), True),
    ("icon_k4.png", "Flat_modern_app_icon__a_solid__2026-08-21T05-47-43.png", (128, 128), True),
    ("icon_k5.png", "Flat_modern_app_icon__a_solid__2026-08-21T05-48-12.png", (128, 128), True),
    ("icon_k6.png", "Flat_modern_app_icon__a_solid__2026-08-21T05-48-41.png", (128, 128), True),
    ("icon_k7.png", "Flat_modern_app_icon__a_solid__2026-08-21T05-49-09.png", (128, 128), True),
    ("icon_k8.png", "Flat_modern_app_icon__a_solid__2026-08-21T05-49-37.png", (128, 128), True),
]

# Final icon texture size (must match tex_pack.py / Desktop.cs draw size).
ICON_SIZE = 128
# Drawn tile size inside the 128 frame. Keeping it < frame leaves a transparent
# margin so the rounded corners are actually visible (a 256->32px downscale of an
# edge-to-edge tile squashes any corner radius to a hard, pixelated edge).
TILE = 100
# Corner radius as a fraction of the tile edge. ~0.23 gives a clearly rounded
# squircle (iOS/Win11-like), not a near-square.
RADIUS_FRAC = 0.23
# Supersample factor for the rounded-rect mask -> smooth, non-stair-stepped edges.
SUPER = 4


def flood_bg_mask(img, T=110):
    """Return a set of (x,y) background pixels via 4-connected flood fill
    seeded from the four corners. `img` is a small RGB image."""
    w, h = img.size
    px = img.load()
    seeds = [(0, 0), (w - 1, 0), (0, h - 1), (w - 1, h - 1)]
    br = sum(px[x, y][0] for x, y in seeds) // 4
    bg = (br,
          sum(px[x, y][1] for x, y in seeds) // 4,
          sum(px[x, y][2] for x, y in seeds) // 4)
    visited = set(seeds)
    dq = deque(seeds)
    while dq:
        x, y = dq.popleft()
        for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            nx, ny = x + dx, y + dy
            if 0 <= nx < w and 0 <= ny < h and (nx, ny) not in visited:
                r, g, b = px[nx, ny]
                if max(abs(r - bg[0]), abs(g - bg[1]), abs(b - bg[2])) < T:
                    visited.add((nx, ny))
                    dq.append((nx, ny))
    return visited, w, h


def rounded_rect_alpha(size, box, radius, supersample=SUPER):
    """Anti-aliased rounded-rectangle alpha mask (L mode, 0=transparent, 255=opaque).

    Drawn at `supersample`x then downscaled with LANCZOS so the corner edges are
    a smooth gradient instead of a stair-stepped pixel boundary. `box` is the
    rectangle in final `size` coordinates; `radius` is in final coordinates."""
    s = supersample
    big = Image.new("L", (size * s, size * s), 0)
    d = ImageDraw.Draw(big)
    bx = [int(v * s) for v in box]
    r = max(1, int(radius * s))
    d.rounded_rectangle(bx, radius=r, fill=255)
    return big.resize((size, size), Image.LANCZOS)


def alpha_from_flood(rgb_full, T=110):
    """Build a smooth, rounded NexOS app icon (ICON_SIZE x ICON_SIZE RGBA).

    The AI tile is recovered by flood-filling the light background that surrounds
    it (seeded from the 4 corners). The AI tile already carries a colour + white
    glyph, but its own corners are small/uneven and its antialiasing is coarse.
    We therefore:
      * scale the recovered tile into a TILE-sized box centred in the frame
        (leaving a transparent margin so the rounding stays visible once the 128
        texture is drawn at 24-32px),
      * paint the tile's dominant colour into a FRESH supersampled rounded-rect
        (radius RADIUS_FRAC*TILE) and lay the AI glyph/art on top -- this makes
        OUR clean, large, anti-aliased corner radius dominate instead of being
        clipped by the AI tile's own smaller corners,
      * a hair of blur for silky edges.
    """
    W = 256
    small = rgb_full.resize((W, W), Image.LANCZOS)
    bg_set, w, h = flood_bg_mask(small, T)
    a = Image.new("L", (w, h), 0)
    apx = a.load()
    for (x, y) in bg_set:
        apx[x, y] = 255
    a = ImageOps.invert(a).filter(ImageFilter.GaussianBlur(1))
    bbox = a.getbbox()
    if bbox is None:
        bbox = (0, 0, w, h)
    # inset a few px to drop the AI's own imperfect edges
    inset = max(3, int(0.02 * w))
    l = min(bbox[0] + inset, bbox[2] - 1)
    t = min(bbox[1] + inset, bbox[3] - 1)
    r = max(bbox[2] - inset, l + 1)
    b = max(bbox[3] - inset, t + 1)

    # tile colour content (RGBA, alpha = recovered tile) scaled into TILE
    content = small.crop((l, t, r, b)).resize((TILE, TILE), Image.LANCZOS).convert("RGBA")
    cpx = content.load()
    # dominant tile colour: average of the central region (avoids the glyph)
    cs = TILE // 4
    rs = gs = bs = n = 0
    for yy in range(cs, TILE - cs):
        for xx in range(cs, TILE - cs):
            if cpx[xx, yy][3] > 128:
                rr, gg, bb, _ = cpx[xx, yy]
                rs += rr; gs += gg; bs += bb; n += 1
    if n == 0:
        fill = (120, 120, 120)
    else:
        fill = (rs // n, gs // n, bs // n)

    # solid tile in the dominant colour, AI art/glyph on top
    tile = Image.new("RGBA", (TILE, TILE), fill + (255,))
    tile.alpha_composite(content)

    # clean supersampled rounded-rect mask over the placed tile box
    radius = max(4, int(RADIUS_FRAC * TILE))
    off = (ICON_SIZE - TILE) // 2
    rmask = rounded_rect_alpha(ICON_SIZE,
                               (off, off, off + TILE, off + TILE),
                               radius, SUPER)
    rmask = rmask.filter(ImageFilter.GaussianBlur(0.5))

    out = Image.new("RGBA", (ICON_SIZE, ICON_SIZE), (0, 0, 0, 0))
    out.paste(tile, (off, off))
    out.putalpha(rmask)
    return out


def main():
    for tgt, src, (W, H), is_icon in TASKS:
        sp = os.path.join(GEN, src)
        if not os.path.exists(sp):
            print("MISSING", sp)
            continue
        im = Image.open(sp)
        if is_icon:
            out = alpha_from_flood(im.convert("RGB"))
            out.save(os.path.join(ASSETS, tgt), "PNG")
            a = out.split()[3]
            opaque = sum(1 for p in a.getdata() if p > 200)
            print("icon %s -> %s  RGBA opaque=%.2f" % (tgt, out.size, opaque / (128 * 128)))
        else:
            out = im.convert("RGB").resize((W, H), Image.LANCZOS)
            out.save(os.path.join(ASSETS, tgt), "PNG")
            print("tex   %s -> %s  %s" % (tgt, out.size, out.mode))
    print("done.")


if __name__ == "__main__":
    main()
