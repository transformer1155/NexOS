import os
import subprocess
import sys

from PIL import Image

URLS = {
    "menu.png": "https://images.unsplash.com/photo-1528459801416-a9e53bbf4e17?w=512&q=80",
    "taskbar.png": "https://images.unsplash.com/photo-1528459801416-a9e53bbf4e17?w=512&q=80",
}

OUT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "assets")
os.makedirs(OUT, exist_ok=True)

for name, url in URLS.items():
    path = os.path.join(OUT, name)
    r = subprocess.run(["curl", "-sS", "-L", "--max-time", "30", "-o", path, url],
                       capture_output=True, timeout=40)
    if r.returncode != 0 or not os.path.exists(path) or os.path.getsize(path) < 1000:
        print("%s FAIL curl rc=%d" % (name, r.returncode))
        sys.exit(1)
    im = Image.open(path).convert("RGB")
    w, h = im.size
    # tex_pack will resize anyway; keep a sensible source size.
    im = im.resize((min(w, 512), min(h, 512)), Image.LANCZOS)
    im.save(path)
    # measure with the exact test threshold (>700)
    px = im.load()
    n = light = 0
    s = 0
    for yy in range(0, im.height, 2):
        for xx in range(0, im.width, 2):
            r2, g, b = px[xx, yy]
            s += r2 + g + b
            n += 1
            if r2 + g + b > 700:
                light += 1
    print("%s  %dx%d avg=%.0f  >700=%.1f%%" % (name, im.width, im.height, s / n, 100.0 * light / n))
print("done")
