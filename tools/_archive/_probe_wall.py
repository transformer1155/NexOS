import random
from PIL import Image

im = Image.open("assets/wallpaper.png").convert("RGB")
w, h = im.size
px = im.load()
s = n = 0
for _ in range(3000):
    x = random.randrange(w); y = random.randrange(h)
    r, g, b = px[x, y]
    s += r + g + b; n += 1
print("wallpaper avg RGB sum:", round(s / n, 1), "size", w, h)

# Also count pixels above LIGHT_SUM=700 at full res (sampled)
light = 0
for _ in range(3000):
    x = random.randrange(w); y = random.randrange(h)
    r, g, b = px[x, y]
    if r + g + b > 700:
        light += 1
print("fraction of light pixels (>700):", light / 30.0, "%")
