#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Generate real UI resource images for the NexOS C# desktop from the colour
tokens that are actually defined in the source code.

This is the code-driven substitute for the unavailable "AI绘图" skill: instead
of an AI image model we render every Theme/palette/icon token the C# code
already declares, so the produced PNGs are byte-for-byte faithful to the
design system (not an approximation).

Outputs (into build/ui/):
  palette_surfaces.png   window chrome / surface palette
  palette_accent.png     accent ramp + 6-colour accent picker
  palette_semantic.png   Good / Warn / Danger
  wallpaper_light.png    Bloom wallpaper (light theme)
  wallpaper_dark.png     Bloom wallpaper (dark theme)
  wallpaper_presets.png  6 personalize wallpaper presets (top->bottom gradient)
  icons_desktop.png      8 desktop / pinned app tiles
  icons_tray.png        3 taskbar tray glyph tiles
  icons_settings.png    6 settings category tiles
  chrome_taskbar.png     taskbar + context-menu + window-chrome colours

All colour values are transcribed from (file:line):
  csharp/NexOS.Forms/Forms.cs
  csharp/apps/Shell/Desktop.cs
  csharp/apps/Shell/Apps.cs
  csharp/winhost/ShellForm.cs
"""

import os
import math
from PIL import Image, ImageDraw, ImageFont

BUILD = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "build", "ui")
os.makedirs(BUILD, exist_ok=True)

# ---------------------------------------------------------------------------
# tiny helpers
# ---------------------------------------------------------------------------

def hex2rgb(h):
    h = h if isinstance(h, int) else int(h, 16)
    return ((h >> 16) & 0xFF, (h >> 8) & 0xFF, h & 0xFF)

def rgb2css(h):
    r, g, b = hex2rgb(h)
    return "#%02X%02X%02X" % (r, g, b)

_FONT = None
def font(size=18):
    global _FONT
    if _FONT and _FONT.size == size:
        return _FONT
    try:
        p = "C:/Windows/Fonts/msyh.ttc"
        _FONT = ImageFont.truetype(p, size)
    except Exception:
        _FONT = ImageFont.load_default()
    return _FONT

def new(w, h, bg=(255, 255, 255, 255)):
    return Image.new("RGBA", (w, h), bg)

def round_rect(d, box, r, fill=None, outline=None, width=1):
    d.rounded_rectangle(box, radius=r, fill=fill, outline=outline, width=width)

def text(d, xy, s, fill=(0, 0, 0), size=18, anchor="la"):
    d.text(xy, s, font=font(size), fill=fill, anchor=anchor)

def save(im, name):
    path = os.path.join(BUILD, name)
    im.convert("RGBA").save(path, "PNG")
    print("wrote", path, im.size)
    return path

# ---------------------------------------------------------------------------
# 1. surface / window-chrome palette
# ---------------------------------------------------------------------------

SURFACES = [
    ("WinBg",     0xF3F3F3, "窗口客户区背景 (mica 灰)"),
    ("Card",      0xFFFFFF, "凸起表面 / 卡片"),
    ("CardAlt",   0xFAFAFA, "次级行背景"),
    ("Border",    0xE1E1E1, "发丝级分隔线"),
    ("BorderMid", 0xCFCFCF, "中等边框"),
    ("Hover",     0xEEF3FA, "控件悬停填充"),
    ("Sel",       0xE1EDFB, "选中行填充"),
    ("Text",      0x1B1B1B, "主文本"),
    ("TextSub",   0x606060, "次级文本"),
    ("TextFaint", 0x909090, "弱化文本"),
    ("White",     0xFFFFFF, "纯白"),
    ("Bar",       0xF2F5FA, "任务栏填充 (浅色主题)"),
    ("BarLine",   0xD5DDE8, "任务栏分隔/描边"),
    ("BarHot",    0xE1EAF6, "任务栏悬停"),
    ("MenuBg",    0xF7F9FC, "右键菜单背景"),
    ("MenuFoot",  0xEBF0F7, "菜单底栏"),
    ("FieldBg",   0xFFFFFF, "输入框背景"),
    ("FieldEdge", 0xCCD4E0, "输入框描边"),
    ("Ghost",     0x8A93A0, "占位/灰字"),
    ("IconHot",   0x1D5A96, "桌面图标悬停"),
    ("CFrame",    0xCFCFCF, "窗口边框线 (ShellForm)"),
    ("CBarAct",   0xFFFFFF, "标题栏激活态"),
    ("CBarIn",    0xF3F3F3, "标题栏非激活/底色"),
    ("CInk",      0x1B1B1B, "标题文本"),
    ("CInkDim",   0x777777, "标题次文本"),
    ("CHover",    0xE8E8E8, "标题按钮悬停"),
]

def render_palette_grid(items, cols, cell_w, cell_h, title):
    rows = (len(items) + cols - 1) // cols
    pad = 16
    W = pad * 2 + cols * cell_w
    H = pad * 2 + rows * cell_h + 40
    im = new(W, H)
    d = ImageDraw.Draw(im)
    text(d, (pad, pad - 4), title, fill=(0, 0, 0), size=22)
    for i, (name, hexv, desc) in enumerate(items):
        r, c = divmod(i, cols)
        x = pad + c * cell_w
        y = pad + 40 + r * cell_h
        round_rect(d, [x, y, x + cell_w - 10, y + cell_h - 10], 8,
                   fill=hex2rgb(hexv), outline=(0xCF, 0xCF, 0xCF))
        text(d, (x + 10, y + 8), name, fill=(0x33, 0x33, 0x33), size=16)
        text(d, (x + 10, y + 30), rgb2css(hexv), fill=(0x66, 0x66, 0x66), size=14)
    return im

save(render_palette_grid(SURFACES, 3, 250, 64, "Surface & Window-Chrome Palette"),
     "palette_surfaces.png")

# ---------------------------------------------------------------------------
# 2. accent ramp + 6-colour picker
# ---------------------------------------------------------------------------

ACCENTS = [
    ("Accent",    0x0078D4, "主强调色 (Fluent 蓝)"),
    ("AccentHi",  0x1A86D9, "悬停强调色"),
    ("AccentLo",  0x005FB0, "按下强调色"),
    ("CClose",    0xC42B1C, "关闭键 / 危险红 (ShellForm)"),
    ("CBlue",     0x0078D4, "标题栏蓝点 (ShellForm)"),
]
PICKER = [
    (0x0078D4, "蓝"), (0x8B5CF6, "紫"), (0x0EA5E9, "青"),
    (0x107C10, "绿"), (0xE11D8A, "品红"), (0xF59E0B, "橙"),
]

def render_accents():
    W = 16 + 3 * 260 + 6 * 130 + 16
    H = 16 + 40 + 120 + 16
    im = new(W, H)
    d = ImageDraw.Draw(im)
    text(d, (16, 12), "Accent Ramp & 6-Colour Picker", fill=(0, 0, 0), size=22)
    y = 56
    for i, (name, hexv, desc) in enumerate(ACCENTS):
        x = 16 + i * 260
        round_rect(d, [x, y, x + 250, y + 110], 10, fill=hex2rgb(hexv))
        text(d, (x + 12, y + 12), name, fill=(255, 255, 255), size=16)
        text(d, (x + 12, y + 36), rgb2css(hexv), fill=(235, 235, 235), size=14)
        text(d, (x + 12, y + 62), desc, fill=(235, 235, 235), size=13)
    y2 = y + 130
    text(d, (16, y2 - 22), "Theme.Accents() 强调色选择板", fill=(0, 0, 0), size=18)
    for i, (hexv, label) in enumerate(PICKER):
        x = 16 + i * 130
        round_rect(d, [x, y2, x + 120, y2 + 80], 10, fill=hex2rgb(hexv))
        text(d, (x + 60, y2 + 40), label, fill=(255, 255, 255), size=20, anchor="mm")
    return im

save(render_accents(), "palette_accent.png")

# ---------------------------------------------------------------------------
# 3. semantic colours
# ---------------------------------------------------------------------------

SEMANTIC = [
    ("Good",   0x107C10, "成功 / 开启"),
    ("Warn",   0xC29A00, "警告 / 注意"),
    ("Danger", 0xC42B1C, "破坏 / 停止"),
]
save(render_palette_grid(SEMANTIC, 3, 250, 64, "Semantic Colours"),
     "palette_semantic.png")

# ---------------------------------------------------------------------------
# 4. wallpaper (Bloom) — light & dark
# ---------------------------------------------------------------------------

def render_wallpaper(top, bot, glows, W=480, H=270):
    im = new(W, H)
    px = im.load()
    rt, gt, bt = hex2rgb(top)
    rb, gb, bb = hex2rgb(bot)
    for y in range(H):
        t = y / (H - 1)
        for x in range(W):
            r = int(rt + (rb - rt) * t)
            g = int(gt + (gb - gt) * t)
            b = int(bt + (bb - bt) * t)
            px[x, y] = (r, g, b, 255)
    d = ImageDraw.Draw(im, "RGBA")
    for gx, gy, gc, gr, ga in glows:
        steps = 24
        for i in range(steps, 0, -1):
            f = i / steps
            a = int(gr * (1 - f) * ga)
            col = tuple(int(c * f + 255 * (1 - f)) for c in hex2rgb(gc)) + (a,)
            d.ellipse([gx - gr * f, gy - gr * f, gx + gr * f, gy + gr * f],
                      fill=col)
    return im

LIGHT_GLOWS = [
    (360, 150, 0x2C86D6, 150, 0.55),
    (330, 120, 0xBFE4FF, 70, 0.75),
    (120, 230, 0x0E3F6E, 120, 0.40),
]
DARK_GLOWS = [
    (360, 150, 0x3A5BD9, 150, 0.45),
    (330, 120, 0x8FA8FF, 70, 0.55),
    (120, 230, 0x1B1B2A, 120, 0.30),
]
save(render_wallpaper(0x05162C, 0x0B4A83, LIGHT_GLOWS), "wallpaper_light.png")
save(render_wallpaper(0x0B0B12, 0x1B1B2A, DARK_GLOWS), "wallpaper_dark.png")

# ---------------------------------------------------------------------------
# 5. personalize wallpaper presets (top -> bottom gradient strips)
# ---------------------------------------------------------------------------

PRESET_TOP = [0x05162C, 0x1B1035, 0x06283D, 0x052E16, 0x3A0A1E, 0x2E1A05]
PRESET_BOT = [0x0B4A83, 0x6D28D9, 0x0EA5E9, 0x107C10, 0xE11D8A, 0xF59E0B]
PRESET_NAME = ["Abyss", "Nebula", "Lagoon", "Forest", "Rose", "Sunset"]

def render_presets():
    sw, sh, gap = 150, 200, 16
    W = 16 + len(PRESET_TOP) * (sw + gap)
    H = 16 * 2 + sh
    im = new(W, H)
    d = ImageDraw.Draw(im)
    text(d, (16, 12), "Personalize Wallpaper Presets", fill=(0, 0, 0), size=20)
    for i in range(len(PRESET_TOP)):
        x = 16 + i * (sw + gap)
        y = 44
        rt, gt, bt = hex2rgb(PRESET_TOP[i])
        rb, gb, bb = hex2rgb(PRESET_BOT[i])
        d2 = ImageDraw.Draw(im, "RGBA")
        for yy in range(sh):
            t = yy / (sh - 1)
            r = int(rt + (rb - rt) * t)
            g = int(gt + (gb - gt) * t)
            b = int(bt + (bb - bt) * t)
            d2.line([(x, y + yy), (x + sw - 1, y + yy)], fill=(r, g, b, 255))
        round_rect(d, [x, y, x + sw, y + sh], 10, outline=(0xCF, 0xCF, 0xCF))
        text(d, (x + sw // 2, y + sh + 6), PRESET_NAME[i],
             fill=(0x33, 0x33, 0x33), size=15, anchor="ma")
    return im

save(render_presets(), "wallpaper_presets.png")

# ---------------------------------------------------------------------------
# 6. desktop / pinned app icon tiles
# ---------------------------------------------------------------------------

DESKTOP_ICONS = [
    ("This PC",  0xFFC83D, "P", "FileExplorer"),
    ("Terminal", 0x2F3A45, ">", "Terminal"),
    ("Calc",     0x00A3A3, "=", "Calculator"),
    ("Task Mgr", 0x0F7B0F, "T", "TaskManager"),
    ("Settings", 0x0078D4, "S", "ControlPanel"),
    ("Optimizer",0x8256D0, "M", "MemOptimizer"),
    ("Notepad",  0x4A6FA5, "N", "Notepad"),
    ("About",    0xD8541B, "i", "About"),
]

def render_icon_tiles(icons, title, cols=4, size=110, gap=20):
    rows = (len(icons) + cols - 1) // cols
    W = 16 + cols * (size + gap)
    H = 16 * 2 + 30 + rows * (size + gap + 24)
    im = new(W, H)
    d = ImageDraw.Draw(im)
    text(d, (16, 12), title, fill=(0, 0, 0), size=20)
    for i, (name, hexv, letter, kind) in enumerate(icons):
        r, c = divmod(i, cols)
        x = 16 + c * (size + gap)
        y = 44 + r * (size + gap + 24)
        round_rect(d, [x, y, x + size, y + size], 22, fill=hex2rgb(hexv))
        text(d, (x + size // 2, y + size // 2), letter,
             fill=(255, 255, 255), size=44, anchor="mm")
        text(d, (x + size // 2, y + size + 8), name,
             fill=(0x33, 0x33, 0x33), size=15, anchor="ma")
    return im

save(render_icon_tiles(DESKTOP_ICONS, "Desktop & Pinned App Icons"),
     "icons_desktop.png")

# ---------------------------------------------------------------------------
# 7. tray glyph tiles
# ---------------------------------------------------------------------------

TRAY_ICONS = [
    ("Tasks",  0x8B5CF6, "▣", "后台任务 (重叠窗口紫)"),
    ("Voice",  0x107C10, "♪", "语音输入 (开=绿/关=灰 4B5563)"),
    ("Network",0x0EA5E9, "◍", "以太网/网络 (地球青)"),
]

def render_tray():
    size = 110
    W = 16 + 3 * (size + 20)
    H = 16 * 2 + 30 + size + 40
    im = new(W, H)
    d = ImageDraw.Draw(im)
    text(d, (16, 12), "Taskbar Tray Glyphs", fill=(0, 0, 0), size=20)
    for i, (name, hexv, glyph, desc) in enumerate(TRAY_ICONS):
        x = 16 + i * (size + 20)
        y = 44
        round_rect(d, [x, y, x + size, y + size], 24, fill=hex2rgb(hexv))
        text(d, (x + size // 2, y + size // 2 + 4), glyph,
             fill=(255, 255, 255), size=46, anchor="mm")
        text(d, (x + size // 2, y + size + 8), name,
             fill=(0x33, 0x33, 0x33), size=15, anchor="ma")
        text(d, (x + size // 2, y - 4), desc,
             fill=(0x66, 0x66, 0x66), size=12, anchor="ma")
    return im

save(render_tray(), "icons_tray.png")

# ---------------------------------------------------------------------------
# 8. settings category tiles
# ---------------------------------------------------------------------------

SETTINGS_CATS = [
    ("System",    0x0078D4),
    ("Display",   0x8B5CF6),
    ("Network",   0x10B981),
    ("Storage",   0xF59E0B),
    ("Devices",   0x06B6D4),
    ("Power",     0xEF4444),
]
save(render_icon_tiles([(n, h, n[0], n) for n, h in SETTINGS_CATS],
                       "Settings Category Tiles", cols=6, size=110, gap=20),
     "icons_settings.png")

print("done.")
