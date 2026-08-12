from PIL import Image

for name in ["assets/menu.png", "assets/taskbar.png", "assets/winbg.png",
             "assets/wallpaper.png", "assets/chrome.png"]:
    im = Image.open(name).convert("RGB")
    w, h = im.size
    px = im.load()
    s = n = 0
    light = 0
    for yy in range(0, h, max(1, h // 200)):
        for xx in range(0, w, max(1, w // 200)):
            r, g, b = px[xx, yy]
            s += r + g + b
            n += 1
            if r + g + b > 700:
                light += 1
    print("%-20s %dx%d avg=%5.0f  light%%=%.1f" % (name, w, h, s / n, 100.0 * light / n))
