from PIL import Image
import collections

for name in ["assets/menu.png", "assets/taskbar.png", "assets/winbg.png",
             "assets/chrome.png"]:
    im = Image.open(name).convert("RGB")
    w, h = im.size
    px = im.load()
    cnt = collections.Counter()
    for yy in range(0, h, 4):
        for xx in range(0, w, 4):
            cnt[(px[xx, yy][0] // 32, px[xx, yy][1] // 32, px[xx, yy][2] // 32)] += 1
    top = cnt.most_common(5)
    print("%-16s %dx%d top5-buckets:" % (name, w, h))
    for (r, g, b), c in top:
        print("   rgb(%d-%d, %d-%d, %d-%d) x%d" % (r * 32, r * 32 + 31, g * 32, g * 32 + 31, b * 32, b * 32 + 31, c))
