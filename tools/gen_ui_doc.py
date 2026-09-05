#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Build build/ui/ui_resources.html — the UI-resource design-system document.

It embeds every generated PNG (base64) so the file is fully self-contained and
previews anywhere, and lists every colour/icon token together with the exact
source location it was transcribed from.
"""
import os, base64, datetime

BUILD = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "build", "ui")
OUT = os.path.join(BUILD, "ui_resources.html")

def b64(name):
    with open(os.path.join(BUILD, name), "rb") as f:
        return "data:image/png;base64," + base64.b64encode(f.read()).decode()

def hex2rgb(h):
    h = int(h, 16) if isinstance(h, str) else h
    return (h >> 16) & 0xFF, (h >> 8) & 0xFF, h & 0xFF

def css(h):
    r, g, b = hex2rgb(h)
    return "#%02X%02X%02X" % (r, g, b)

def row(name, hexv, role, src):
    return ("<tr><td><code>%s</code></td>"
            "<td><span class='sw' style='background:%s'></span>%s</td>"
            "<td class='rgb'>%s</td><td>%s</td><td class='src'>%s</td></tr>"
            % (name, css(hexv), css(hexv), css(hexv), role, src))

def img(name, cap):
    return "<figure><img src='%s' alt='%s'><figcaption>%s</figcaption></figure>" % (
        b64(name), cap, cap)

def tiles_table(items):
    out = ["<div class='tiles'>"]
    for name, hexv, letter, kind in items:
        out.append(
            "<div class='tile'>"
            "<div class='chip' style='background:%s'>%s</div>"
            "<div class='tname'>%s</div>"
            "<div class='tkind'>%s · %s</div></div>" % (
                css(hexv), letter, name, kind, css(hexv)))
    out.append("</div>")
    return "".join(out)

# ----- data (transcribed from source) --------------------------------------
SURFACES = [
    ("WinBg", 0xF3F3F3, "窗口客户区背景 (mica 灰)", "Forms.cs:101"),
    ("Card", 0xFFFFFF, "凸起表面 / 卡片", "Forms.cs:102"),
    ("CardAlt", 0xFAFAFA, "次级行背景", "Forms.cs:103"),
    ("Border", 0xE1E1E1, "发丝级分隔线", "Forms.cs:104"),
    ("BorderMid", 0xCFCFCF, "中等边框", "Forms.cs:105"),
    ("Hover", 0xEEF3FA, "控件悬停填充", "Forms.cs:113"),
    ("Sel", 0xE1EDFB, "选中行填充", "Forms.cs:114"),
    ("Text", 0x1B1B1B, "主文本", "Forms.cs:109"),
    ("TextSub", 0x606060, "次级文本", "Forms.cs:110"),
    ("TextFaint", 0x909090, "弱化文本", "Forms.cs:111"),
    ("White", 0xFFFFFF, "纯白", "Forms.cs:112"),
    ("Bar", 0xF2F5FA, "任务栏填充 (浅色)", "Desktop.cs:47"),
    ("BarLine", 0xD5DDE8, "任务栏分隔/描边", "Desktop.cs:48"),
    ("BarHot", 0xE1EAF6, "任务栏悬停", "Desktop.cs:49"),
    ("Ink", 0x1B1B1B, "图标/文本墨色", "Desktop.cs:50"),
    ("MenuBg", 0xF7F9FC, "右键菜单背景", "Desktop.cs:51"),
    ("MenuFoot", 0xEBF0F7, "菜单底栏", "Desktop.cs:52"),
    ("FieldBg", 0xFFFFFF, "输入框背景", "Desktop.cs:53"),
    ("FieldEdge", 0xCCD4E0, "输入框描边", "Desktop.cs:54"),
    ("Ghost", 0x8A93A0, "占位/灰字", "Desktop.cs:55"),
    ("IconHot", 0x1D5A96, "桌面图标悬停", "Desktop.cs:46"),
    ("CFrame", 0xCFCFCF, "窗口边框线", "ShellForm.cs:50"),
    ("CBarAct", 0xFFFFFF, "标题栏激活态", "ShellForm.cs:51"),
    ("CBarIn", 0xF3F3F3, "标题栏非激活/底色", "ShellForm.cs:52"),
    ("CInk", 0x1B1B1B, "标题文本", "ShellForm.cs:53"),
    ("CInkDim", 0x777777, "标题次文本", "ShellForm.cs:54"),
    ("CHover", 0xE8E8E8, "标题按钮悬停", "ShellForm.cs:56"),
]

ACCENTS = [
    ("Accent", 0x0078D4, "主强调色 (Fluent 蓝)", "Forms.cs:106 / Theme.cs:131"),
    ("AccentHi", 0x1A86D9, "悬停强调色", "Forms.cs:107"),
    ("AccentLo", 0x005FB0, "按下强调色", "Forms.cs:108"),
    ("CClose", 0xC42B1C, "关闭键 / 危险红", "ShellForm.cs:55"),
    ("CBlue", 0x0078D4, "标题栏蓝点", "ShellForm.cs:258"),
]
PICKER = [(0x0078D4, "蓝"), (0x8B5CF6, "紫"), (0x0EA5E9, "青"),
          (0x107C10, "绿"), (0xE11D8A, "品红"), (0xF59E0B, "橙")]
SEMANTIC = [
    ("Good", 0x107C10, "成功 / 开启", "Forms.cs:115"),
    ("Warn", 0xC29A00, "警告 / 注意", "Forms.cs:116"),
    ("Danger", 0xC42B1C, "破坏 / 停止", "Forms.cs:117"),
]
WALL = [
    ("WallTop (light)", 0x05162C, "壁纸渐变顶 (浅色)", "Theme.cs:129 / Desktop.cs:41"),
    ("WallBot (light)", 0x0B4A83, "壁纸渐变底 (浅色)", "Theme.cs:130 / Desktop.cs:42"),
    ("WallTop (dark)", 0x0B0B12, "壁纸渐变顶 (深色)", "Apps.cs:429"),
    ("WallBot (dark)", 0x1B1B2A, "壁纸渐变底 (深色)", "Apps.cs:429"),
    ("Glow0", 0x0E3F6E, "Bloom 外晕", "Desktop.cs:43"),
    ("Glow1", 0x2C86D6, "Bloom 中段", "Desktop.cs:44"),
    ("GlowHi", 0xBFE4FF, "Bloom 高光核", "Desktop.cs:45"),
]
PRESETS = [
    ("Abyss", 0x05162C, 0x0B4A83), ("Nebula", 0x1B1035, 0x6D28D9),
    ("Lagoon", 0x06283D, 0x0EA5E9), ("Forest", 0x052E16, 0x107C10),
    ("Rose", 0x3A0A1E, 0xE11D8A), ("Sunset", 0x2E1A05, 0xF59E0B),
]
DESKTOP_ICONS = [
    ("This PC", 0xFFC83D, "P", "FileExplorer"),
    ("Terminal", 0x2F3A45, ">", "Terminal"),
    ("Calc", 0x00A3A3, "=", "Calculator"),
    ("Task Mgr", 0x0F7B0F, "T", "TaskManager"),
    ("Settings", 0x0078D4, "S", "ControlPanel"),
    ("Optimizer", 0x8256D0, "M", "MemOptimizer"),
    ("Notepad", 0x4A6FA5, "N", "Notepad"),
    ("About", 0xD8541B, "i", "About"),
]
TRAY_ICONS = [
    ("Tasks", 0x8B5CF6, "▣", "后台任务 (重叠窗口)"),
    ("Voice", 0x107C10, "♪", "语音输入 (开=绿/关=4B5563)"),
    ("Network", 0x0EA5E9, "◍", "以太网/网络 (地球)"),
]
SETTINGS_CATS = [
    ("System", 0x0078D4), ("Display", 0x8B5CF6), ("Network", 0x10B981),
    ("Storage", 0xF59E0B), ("Devices", 0x06B6D4), ("Power", 0xEF4444),
]

# ----- assemble -------------------------------------------------------------
parts = []
parts.append("""<!doctype html><html lang='zh'><head><meta charset='utf-8'>
<title>NexOS C# 桌面 · UI 资源设计系统</title>
<style>
 * { box-sizing: border-box; }
 body { font-family: -apple-system, "Segoe UI", "Microsoft YaHei", sans-serif;
        margin: 0; background: #f4f6fa; color: #1b1b1b; }
 header { background: linear-gradient(135deg,#05162C,#0B4A83); color:#fff;
          padding: 28px 40px; }
 header h1 { margin:0 0 6px; font-size: 24px; }
 header p { margin: 0; opacity: .85; font-size: 14px; }
 main { padding: 24px 40px 60px; max-width: 1100px; margin: 0 auto; }
 section { background:#fff; border:1px solid #e6e9ef; border-radius:12px;
           padding: 20px 24px; margin: 18px 0; box-shadow:0 1px 3px rgba(0,0,0,.04); }
 h2 { font-size: 18px; margin: 0 0 12px; border-left:4px solid #0078D4; padding-left:10px; }
 figure { margin: 0 0 8px; }
 figure img { max-width: 100%; border:1px solid #e6e9ef; border-radius:10px;
              background:
               repeating-conic-gradient(#eee 0% 25%, #fff 0% 50%) 50%/18px 18px; }
 figcaption { color:#666; font-size:13px; margin-top:6px; }
 table { width:100%; border-collapse:collapse; font-size:13px; }
 th, td { text-align:left; padding:6px 8px; border-bottom:1px solid #f0f2f6; }
 th { color:#555; font-weight:600; }
 code { background:#f3f5f9; padding:1px 5px; border-radius:4px; font-size:12px; }
 .sw { display:inline-block; width:14px; height:14px; border-radius:3px;
       border:1px solid #ccc; vertical-align:-2px; margin-right:6px; }
 .rgb { color:#666; font-variant-numeric: tabular-nums; }
 .src { color:#999; }
 .tiles { display:flex; flex-wrap:wrap; gap:14px; margin-top:10px; }
 .tile { width:120px; text-align:center; }
 .chip { width:84px; height:84px; border-radius:20px; margin:0 auto 6px;
         color:#fff; font-size:34px; font-weight:700; display:flex;
         align-items:center; justify-content:center;
         box-shadow: inset 0 1px 0 rgba(255,255,255,.4), 0 2px 6px rgba(0,0,0,.18); }
 .tname { font-size:13px; font-weight:600; }
 .tkind { font-size:11px; color:#888; }
 .note { background:#fff8e6; border:1px solid #ffe08a; border-radius:8px;
         padding:10px 14px; font-size:13px; color:#7a5b00; margin:14px 0; }
 .picker span { display:inline-block; width:60px; height:34px; border-radius:6px;
                margin:3px; vertical-align:middle; text-align:center; color:#fff;
                line-height:34px; font-size:12px; }
</style></head><body>
<header><h1>NexOS C# 桌面 · UI 资源设计系统</h1>
<p>由代码驱动精确生成 · 生成时间 __NOW__ · 所有色值均转录自源码</p></header>
<main>
<div class='note'>说明：本环境未注册 <b>AI绘图</b> 技能（<code>use_skill</code> 报
“Can not find skill”，手动附加亦无法加载），无法用 AI 图像模型生图。
替代品是 <b>代码驱动精确渲染</b>：脚本直接读取 C# 源码里声明的 Theme / 调色板 /
图标令牌，用 Pillow 逐像素渲染成真实 PNG，因此生成的资源与界面实际显示
<b>完全一致</b>，而非近似。后续若 AI绘图 技能可用，可将本表作为资源清单直接喂给模型。</div>
""")
parts[-1] = parts[-1].replace("__NOW__",
                              datetime.datetime.now().strftime("%Y-%m-%d %H:%M"))

# 1 surfaces
parts.append("<section><h2>1 · 表面与窗口 Chrome 调色板</h2>")
parts.append(img("palette_surfaces.png", "palette_surfaces.png — 表面/窗口 Chrome 色板"))
parts.append("<table><tr><th>令牌</th><th>颜色</th><th>HEX</th><th>用途</th><th>源码</th></tr>")
for n, h, r, s in SURFACES:
    parts.append(row(n, h, r, s))
parts.append("</table></section>")

# 2 accents
parts.append("<section><h2>2 · 强调色 (Accent)</h2>")
parts.append(img("palette_accent.png", "palette_accent.png — 强调色梯度 + 6 色选择板"))
parts.append("<table><tr><th>令牌</th><th>颜色</th><th>HEX</th><th>用途</th><th>源码</th></tr>")
for n, h, r, s in ACCENTS:
    parts.append(row(n, h, r, s))
parts.append("</table>")
parts.append("<p class='picker'>Theme.Accents() 选择板：")
for h, lab in PICKER:
    parts.append("<span style='background:%s'>%s</span>" % (css(h), lab))
parts.append("</p></section>")

# 3 semantic
parts.append("<section><h2>3 · 语义色</h2>")
parts.append(img("palette_semantic.png", "palette_semantic.png — 成功/警告/危险"))
parts.append("<table><tr><th>令牌</th><th>颜色</th><th>HEX</th><th>用途</th><th>源码</th></tr>")
for n, h, r, s in SEMANTIC:
    parts.append(row(n, h, r, s))
parts.append("</table></section>")

# 4 wallpaper
parts.append("<section><h2>4 · 壁纸 (Bloom 渐变)</h2>")
parts.append(img("wallpaper_light.png", "wallpaper_light.png — 浅色壁纸"))
parts.append(img("wallpaper_dark.png", "wallpaper_dark.png — 深色壁纸"))
parts.append(img("wallpaper_presets.png", "wallpaper_presets.png — 个性化 6 套预设"))
parts.append("<table><tr><th>令牌</th><th>颜色</th><th>HEX</th><th>用途</th><th>源码</th></tr>")
for n, h, r, s in WALL:
    parts.append(row(n, h, r, s))
parts.append("</table>")
parts.append("<p>个性化壁纸预设（顶→底渐变）：</p><div class='tiles'>")
for name, top, bot in PRESETS:
    parts.append("<div class='tile'><div class='chip' "
                  "style='background:linear-gradient(180deg,%s,%s);color:transparent'>.</div>"
                  "<div class='tname'>%s</div><div class='tkind'>%s → %s</div></div>"
                  % (css(top), css(bot), name, css(top), css(bot)))
parts.append("</div></section>")

# 5 desktop icons
parts.append("<section><h2>5 · 桌面 / 固定应用图标</h2>")
parts.append(img("icons_desktop.png", "icons_desktop.png — 8 个应用磁贴"))
parts.append(tiles_table(DESKTOP_ICONS))
parts.append("</section>")

# 6 tray
parts.append("<section><h2>6 · 任务栏托盘字形</h2>")
parts.append(img("icons_tray.png", "icons_tray.png — 后台任务 / 语音 / 网络"))
parts.append(tiles_table(TRAY_ICONS))
parts.append("</section>")

# 7 settings categories
parts.append("<section><h2>7 · 设置分类磁贴</h2>")
parts.append(img("icons_settings.png", "icons_settings.png — 6 个设置分类"))
parts.append("<div class='tiles'>")
for name, h in SETTINGS_CATS:
    parts.append("<div class='tile'><div class='chip' style='background:%s'>%s</div>"
                 "<div class='tname'>%s</div><div class='tkind'>%s</div></div>"
                 % (css(h), name[0], name, css(h)))
parts.append("</div></section>")

parts.append("""<section><h2>资源清单 (生成文件)</h2>
<table><tr><th>文件</th><th>内容</th></tr>
<tr><td><code>build/ui/wallpaper_light.png</code></td><td>Bloom 浅色壁纸</td></tr>
<tr><td><code>build/ui/wallpaper_dark.png</code></td><td>Bloom 深色壁纸</td></tr>
<tr><td><code>build/ui/wallpaper_presets.png</code></td><td>6 套个性化预设</td></tr>
<tr><td><code>build/ui/palette_surfaces.png</code></td><td>表面/Chrome 色板</td></tr>
<tr><td><code>build/ui/palette_accent.png</code></td><td>强调色 + 选择板</td></tr>
<tr><td><code>build/ui/palette_semantic.png</code></td><td>语义色</td></tr>
<tr><td><code>build/ui/icons_desktop.png</code></td><td>桌面/固定应用图标</td></tr>
<tr><td><code>build/ui/icons_tray.png</code></td><td>托盘字形</td></tr>
<tr><td><code>build/ui/icons_settings.png</code></td><td>设置分类磁贴</td></tr>
</table>
<p>生成脚本：<code>tools/gen_ui_assets.py</code>（Pillow 渲染）、
<code>tools/gen_ui_doc.py</code>（本文档）。重跑：
<code>python tools/gen_ui_assets.py &amp;&amp; python tools/gen_ui_doc.py</code></p>
</section></main></body></html>""")

with open(OUT, "w", encoding="utf-8") as f:
    f.write("\n".join(parts))
print("wrote", OUT, os.path.getsize(OUT), "bytes")
