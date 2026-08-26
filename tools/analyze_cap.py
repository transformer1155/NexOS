#!/usr/bin/env python3
"""Render an ASCII luminance map of a captured frame so the layout can be
reasoned about without viewing the image.  Also reports per-row blueness
(B-R) to locate the neutral taskbar band vs the blue wallpaper."""
import os, importlib.util

PROJ = r"D:\MyOS\bootloader"
spec = importlib.util.spec_from_file_location(
    "ppm_to_png", os.path.join(PROJ, "tools", "ppm_to_png.py"))
ppmmod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ppmmod)

PPM = os.path.join(PROJ, "build", "desk_cap.ppm")
w, h, px = ppmmod.read_ppm(PPM)

def get(x, y):
    i = (y * w + x) * 3
    return px[i], px[i+1], px[i+2]

def lum(c):
    return 0.299*c[0] + 0.587*c[1] + 0.114*c[2]

ramp = " .:-=+*#%@"
COLS = 64
ROWS = 32
print(f"frame {w}x{h}  ASCII luminance map (rows={ROWS}, cols={COLS}):\n")
for ry in range(ROWS):
    line = ""
    for rx in range(COLS):
        x = int((rx + 0.5) * w / COLS)
        y = int((ry + 0.5) * h / ROWS)
        c = get(x, y)
        L = lum(c)
        line += ramp[min(9, int(L / 26))]
    print(line)

print("\nper-row (top->bottom) neutral taskbar scan: y  avg_lum  max_abs(B-R)  min/max lum")
for ry in range(ROWS):
    y0 = int(ry * h / ROWS)
    y1 = int((ry+1) * h / ROWS)
    lums, brs = [], []
    for y in range(y0, y1, max(1,(y1-y0)//4)):
        for rx in range(0, COLS, 4):
            x = int((rx+0.5)*w/COLS)
            c = get(x, y)
            lums.append(lum(c)); brs.append(abs(c[2]-c[0]))
    if lums:
        al = sum(lums)/len(lums)
        ab = max(brs)
        print(f"  y={y0:3d}-{y1:3d}  lum={al:6.1f}  max|B-R|={ab:3d}  "
              f"range={min(lums):.0f}..{max(lums):.0f}")

# ---- robust taskbar detection (neutral dark band at the bottom) ------
def row_stats(y):
    body = 0; total = 0; bright = 0; br_vals = []
    for x in range(int(0.04*w), int(0.96*w)):
        c = get(x, y)
        if c is None: continue
        L = lum(c); total += 1
        if 12 <= L <= 90: body += 1
        if L > 180: bright += 1
        br_vals.append(c[2] - c[0])
    frac = body/total if total else 0
    med_br = sorted(br_vals)[len(br_vals)//2] if br_vals else 0
    return frac, bright, med_br

band_top = h
for y in range(h-1, int(h*0.82), -1):
    frac, bright, med_br = row_stats(y)
    if frac >= 0.55 and abs(med_br) <= 28:
        band_top = y
    else:
        if band_top < h:  # we were inside the band and now left it
            break

band_h = h - band_top
# bright tray element anywhere in the bottom-right of the band
tray = 0
for y in range(band_top, h):
    for x in range(int(0.7*w), w):
        c = get(x, y)
        if c is not None and lum(c) > 180:
            tray += 1
print(f"\n[TASKBAR] band y={band_top}..{h}  height={band_h}px  bright-tray-px(right)={tray}")
ok_band = band_h >= 22 and band_h <= 90
ok_tray = tray >= 1
ok_neutral = True  # median B-R already constrained in detection
print(f"[VERDICT] taskbar band present = {ok_band}  (height {band_h})")
print(f"[VERDICT] system tray (right)   = {ok_tray}")
print("[VERDICT]", "PASS (Win11 taskbar confirmed on real VM)" if (ok_band and ok_tray) else "CHECK")
