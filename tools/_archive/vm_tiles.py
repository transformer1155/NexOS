import importlib.util, os, sys
PROJ = r"D:\MyOS\bootloader"
spec = importlib.util.spec_from_file_location("p", os.path.join(PROJ, "tools", "ppm_to_png.py"))
m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)

path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(PROJ, "build", "bios_cap2.ppm")
w, h, px = m.read_ppm(path)
print("size", w, h)

def get(x, y):
    i = (y * w + x) * 3
    return px[i], px[i + 1], px[i + 2]

# tile colors (kind -> rgb) from Desktop.cs LoadDefaults / KindStyle
tiles = {
    "This PC   ": (0xFF, 0xC8, 0x3D),
    "Terminal  ": (0x2F, 0x3A, 0x45),
    "Calculator": (0x00, 0xA3, 0xA3),
    "Task Mgr  ": (0x0F, 0x7B, 0x0F),
    "Settings  ": (0x00, 0x78, 0xD4),
    "Optimizer ": (0x82, 0x56, 0xD0),
    "Notepad   ": (0x4A, 0x6F, 0xA5),
    "About     ": (0xD8, 0x54, 0x1B),
    "Browser   ": (0x1A, 0x73, 0xE8),
    "AI Setup  ": (0x6A, 0x3E, 0xA1),
    "AI Agent  ": (0x8A, 0x5C, 0xF6),
    "Demo      ": (0x8A, 0x5C, 0xF6),
}

def near(r, g, b, t, tol=28):
    return abs(r - t[0]) <= tol and abs(g - t[1]) <= tol and abs(b - t[2]) <= tol

from collections import Counter
counts = Counter()
wallpaper = login_card = 0
step = 2
for y in range(0, h, step):
    for x in range(0, w, step):
        r, g, b = get(x, y)
        # C# wallpaper gradient top (dark blue ~0x05162C) or bottom (~0x0B4A83)
        if (r <= 20 and 8 <= g <= 80 and 40 <= b <= 140): wallpaper += 1
        # login card 0x17171B
        if abs(r - 0x17) <= 6 and abs(g - 0x17) <= 6 and abs(b - 0x1B) <= 6: login_card += 1
        for name, t in tiles.items():
            if near(r, g, b, t):
                counts[name] += 1
                break

print("wallpaper-ish px:", wallpaper)
print("login-card px   :", login_card)
print("tile color hits:")
for name, c in counts.most_common():
    print(f"   {name} {c}")
n_tiles = sum(counts.values())
print("total tile px:", n_tiles)
print("VERDICT:", "DESKTOP-ICONS" if n_tiles > 500 else ("LOGIN" if login_card > 2000 else "UNKNOWN"))
