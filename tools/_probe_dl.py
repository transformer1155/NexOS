import os
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
try:
    from PIL import Image
except ImportError:
    Image = None

CANDIDATES = {
    # menu: light subtle surfaces
    "menu_marble":  "https://images.unsplash.com/photo-1560518883-ce09059eeffa?w=512&q=80",
    "menu_stone":   "https://images.unsplash.com/photo-1528459801416-a9e53bbf4e17?w=512&q=80",
    "menu_linen":   "https://images.unsplash.com/photo-1519710164239-da123dc03ef4?w=512&q=80",
    "menu_paper":   "https://images.unsplash.com/photo-1455390582262-044cdead277a?w=512&q=80",
    # taskbar: light glass/metal
    "task_glass":   "https://images.unsplash.com/photo-1513002749550-c59d786b8e6c?w=512&q=80",
    "task_alum":    "https://images.unsplash.com/photo-1557682250-33bd709cbe85?w=512&q=80",
    "task_clouds":  "https://images.unsplash.com/photo-1506744038136-46273834b3fb?w=512&q=80",
}

tmp = tempfile.mkdtemp()
for name, url in CANDIDATES.items():
    path = os.path.join(tmp, name + ".jpg")
    try:
        r = subprocess.run(["curl", "-sS", "-L", "--max-time", "20", "-o", path, url],
                           capture_output=True, timeout=30)
        if r.returncode != 0 or not os.path.exists(path) or os.path.getsize(path) < 1000:
            print("%-12s FAIL (curl rc=%d)" % (name, r.returncode))
            continue
        im = Image.open(path).convert("RGB")
        w, h = im.size
        px = im.load()
        s = n = light = 0
        for yy in range(0, h, max(1, h // 150)):
            for xx in range(0, w, max(1, w // 150)):
                r2, g, b = px[xx, yy]
                s += r2 + g + b
                n += 1
                if r2 + g + b > 600:
                    light += 1
        avg = s / n
        print("%-12s %dx%d avg=%5.0f  light%%=%.1f  %s" %
              (name, w, h, avg, 100.0 * light / n,
               "GOOD" if avg > 170 and 100.0 * light / n > 30 else "check"))
    except Exception as e:
        print("%-12s ERR %s" % (name, e))
