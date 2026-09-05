#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Pack PNG assets into NexOS TEX textures and drop them into sfs_files/.

Texture ids (agreed with gui.cpp / Desktop.cs):
   0    wallpaper  960x540 RGB565   -> sfs_files/tex_wall.tex
   1    taskbar    256x48  RGB565   -> sfs_files/tex_task.tex
   2    start menu 256x128 RGB565   -> sfs_files/tex_menu.tex
   3    title bar  256x32  RGB565   -> sfs_files/tex_chrome.tex
   4    window bg  128x128 RGB565   -> sfs_files/tex_winbg.tex
   100+ icon_k<kind> 128x128 ARGB32 -> sfs_files/tex_k<kind>.tex

Any asset missing from assets/ is generated procedurally (gradient + bloom +
noise), so a build works offline.  Drop real PNGs into assets/ to override:
   wallpaper.png taskbar.png menu.png chrome.png winbg.png icon_k0..k8.png

TEX header (little-endian): magic "TEX1" u32 version u32 width u32 height
u32 format u32 data_bytes then rows of pixels, top-down.
format 0 = RGB565 (2 B/px), format 1 = ARGB32 (4 B/px, [B G R A]).
"""
import os
import random
import struct
import sys

try:
    from PIL import Image, ImageDraw, ImageFilter, ImageFont
except ImportError:  # build-host dependent
    # Pillow is optional.  gui.cpp falls back to procedural drawing when the
    # tex_*.tex files are absent, so a build host without Pillow must still be
    # able to produce a bootable image instead of failing the whole `make`.
    sys.stderr.write("[tex_pack] Pillow not installed - skipping texture "
                     "generation (GUI falls back to procedural theme)\n")
    sys.exit(0)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASSETS = os.path.join(ROOT, "assets")
OUT = os.path.join(ROOT, "sfs_files")

KINDS = [
    (0, "ControlPanel", 0x0078D4, 'S'),
    (1, "FileExplorer", 0xFFC83D, 'P'),
    (2, "TaskManager",  0x0F7B0F, 'T'),
    (3, "Terminal",     0x2F3A45, '>'),
    (4, "Calculator",   0x00A3A3, '='),
    (5, "About",        0xD8541B, 'i'),
    (6, "MemOptimizer", 0x8256D0, 'M'),
    (7, "Notepad",      0x4A6FA5, 'N'),
    (8, "Browser",      0x1A73E8, 'B'),
]


def tex_header(w, h, fmt, nbytes):
    return b"TEX1" + struct.pack("<IIIII", 1, w, h, fmt, nbytes)


def write_tex(path, img, fmt):
    if fmt == 0:
        rgb = img.convert("RGB")
        w, h = rgb.size
        data = bytearray()
        for y in range(h):
            for x in range(w):
                r, g, b = rgb.getpixel((x, y))
                v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
                data += struct.pack("<H", v)
    else:
        rgba = img.convert("RGBA")
        w, h = rgba.size
        data = bytearray()
        for y in range(h):
            for x in range(w):
                r, g, b, a = rgba.getpixel((x, y))
                data += struct.pack("<BBBB", b, g, r, a)
    with open(path, "wb") as f:
        f.write(tex_header(w, h, fmt, len(data)))
        f.write(data)
    print("  %-16s %dx%d fmt=%d %8d B" % (os.path.basename(path), w, h, fmt, len(data)))


def gradient_image(w, h, top, bot):
    g = Image.linear_gradient("L").resize((w, h), Image.BILINEAR)
    chans = []
    for i in range(3):
        a, b = top[i], bot[i]
        chans.append(g.point(lambda v, a=a, b=b: a + (b - a) * v // 255))
    return Image.merge("RGB", chans)


def blend_noise(img, amount=12):
    w, h = img.size
    random.seed(1234)
    n = Image.new("L", (max(1, w // 4), max(1, h // 4)), 128)
    np = n.load()
    for y in range(n.height):
        for x in range(n.width):
            np[x, y] = random.randint(128 - amount, 128 + amount)
    n = n.resize((w, h), Image.BILINEAR).convert("RGB")
    return Image.blend(img, n, 0.05)


def gen_wallpaper(w=960, h=540):
    top, bot = (0x05, 0x16, 0x2C), (0x0B, 0x4A, 0x83)
    img = gradient_image(w, h, top, bot).convert("RGBA")
    glow = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(glow)
    cx, cy, r0 = w // 2, int(h * 0.44), int(h * 0.46)
    d.ellipse([cx - r0, cy - r0, cx + r0, cy + r0], fill=(190, 225, 255, 255))
    glow = glow.filter(ImageFilter.GaussianBlur(r0 // 2))
    r, g, b, a = glow.split()
    a = a.point(lambda v: v * 3 // 10)
    glow = Image.merge("RGBA", (r, g, b, a))
    img = Image.alpha_composite(img, glow).convert("RGB")
    return blend_noise(img, 10)


def gen_surface(w, h, top, bot):
    img = gradient_image(w, h, top, bot)
    return blend_noise(img, 8)


def gen_icon(color, letter, size=64):
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([2, 2, size - 2, size - 2], radius=12, fill=color)
    glow = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    d2 = ImageDraw.Draw(glow)
    d2.ellipse([4, 4, size - 4, size - 4], fill=(255, 255, 255, 90))
    glow = glow.filter(ImageFilter.GaussianBlur(4))
    img = Image.alpha_composite(img, glow)
    font = None
    for p in ("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
              "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
              "C:/Windows/Fonts/segoeuib.ttf",
              "C:/Windows/Fonts/arialbd.ttf"):
        if os.path.exists(p):
            try:
                font = ImageFont.truetype(p, size // 2)
                break
            except Exception:
                font = None
    d = ImageDraw.Draw(img)
    if font is None:
        d.text((size // 2 - 8, size // 2 - 12), letter, fill=(255, 255, 255, 255))
    else:
        d.text((size // 2, size // 2), letter, font=font,
               fill=(255, 255, 255, 255), anchor="mm")
    return img


def load_or_gen(name, size, gen, keep_alpha=False):
    path = os.path.join(ASSETS, name)
    if os.path.isfile(path):
        try:
            im = Image.open(path)
            if size is not None:
                if keep_alpha:
                    im = im.convert("RGBA").resize(size, Image.LANCZOS)
                else:
                    im = im.convert("RGB").resize(size, Image.LANCZOS)
            print("  using asset %s" % path)
            return im
        except Exception as e:
            print("  WARN %s unreadable (%s), generating" % (path, e))
    return gen()


def main():
    if not os.path.isdir(OUT):
        os.makedirs(OUT)
    if not os.path.isdir(ASSETS):
        os.makedirs(ASSETS)

    produced = set()

    def pack(name, fmt, im):
        path = os.path.join(OUT, "tex_%s.tex" % name)
        write_tex(path, im, fmt)
        produced.add(os.path.basename(path))

    print("[tex_pack] generating textures into %s" % OUT)
    pack("wall", 0, load_or_gen("wallpaper.png", (960, 540), gen_wallpaper))
    pack("task", 0, load_or_gen("taskbar.png", (256, 48),
                                lambda: gen_surface(256, 48, (0xF2, 0xF5, 0xFA), (0xE4, 0xEA, 0xF4))))
    pack("menu", 0, load_or_gen("menu.png", (256, 128),
                                lambda: gen_surface(256, 128, (0xF7, 0xF9, 0xFC), (0xEE, 0xF2, 0xF8))))
    pack("chrome", 0, load_or_gen("chrome.png", (256, 32),
                                  lambda: gen_surface(256, 32, (0xFF, 0xFF, 0xFF), (0xEE, 0xEE, 0xEE))))
    pack("winbg", 0, load_or_gen("winbg.png", (128, 128),
                                 lambda: gen_surface(128, 128, (0xF3, 0xF3, 0xF3), (0xE9, 0xE9, 0xE9))))

    for kind, _name, col, letter in KINDS:
        pack("k%d" % kind, 1,
             load_or_gen("icon_k%d.png" % kind, (128, 128),
                         lambda c=col, l=letter: gen_icon(c, l, size=128),
                         keep_alpha=True))

    stale = 0
    for f in sorted(os.listdir(OUT)):
        if f.startswith("tex_") and f.endswith(".tex") and f not in produced:
            os.remove(os.path.join(OUT, f))
            stale += 1
    if stale:
        print("[tex_pack] removed %d stale tex file(s)" % stale)
    print("[tex_pack] done: %d textures" % len(produced))
    return 0


if __name__ == "__main__":
    sys.exit(main())
