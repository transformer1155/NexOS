import sys
from PIL import Image
from collections import Counter

path = sys.argv[1]
im = Image.open(path).convert("RGB")
w, h = im.size
print("size", w, h)
px = im.load()

def near(r, g, b, t, tol=10):
    return abs(r - t[0]) <= tol and abs(g - t[1]) <= tol and abs(b - t[2]) <= tol

targets = {
    "PopupBG   (0xF7F9FC)": (0xF7, 0xF9, 0xFC),
    "PopupHover(0xE7EEF8)": (0xE7, 0xEE, 0xF8),
    "PopupEdge (0xD5DDE8)": (0xD5, 0xDD, 0xE8),
    "DangerRed (0xC42B1C)": (0xC4, 0x2B, 0x1C),
    "MenuTex   (0x1B1B1B)": (0x1B, 0x1B, 0x1B),
    "AccentBlue(0x0078D4)": (0x00, 0x78, 0xD4),
}
counts = Counter()
for y in range(0, h, 2):
    for x in range(0, w, 2):
        r, g, b = px[x, y]
        for name, t in targets.items():
            if near(r, g, b, t, 10):
                counts[name] += 1
                break
for name, c in counts.most_common():
    print(f"  {name} {c}")
has_popup = counts["PopupBG   (0xF7F9FC)"] > 300 and counts["DangerRed (0xC42B1C)"] > 5
print("VERDICT:", "POPUP-VISIBLE" if has_popup else "NO-POPUP")
