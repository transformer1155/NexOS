#!/usr/bin/env python3
"""Capture the REAL BIOS VM (os.img) and verify the new Win11 window edge in
the native 64-bit gui.cpp draw_window(), WITHOUT assuming window coordinates.

Pipeline:
  * boot os.img under QEMU (tcg), wait for GUI, screendump
  * auto-locate the demo window: its body is neutral 0xF3F3F3 (243), which is
    distinct from the bluish portal desktop (B>R, R<=239) and white cards (255)
  * profile a scanline through the found window:
      - body is light and neutral
      - a soft shadow just outside the frame whose darkness *decreases* with
        distance from the frame (gradient, not a uniform ring)
      - no saturated blue "glow" ring (the old rejected look)
"""
import os, sys, time, socket, subprocess

PROJ = r"D:\MyOS\bootloader"
BUILD = os.path.join(PROJ, "build")
QEMU = r"D:\qemu\qemu-system-x86_64.exe"
DISK = os.path.join(BUILD, "os.img")
SERIAL = os.path.join(BUILD, "serial_edge.txt")
MON_PORT = 4579
PPM = os.path.join(BUILD, "edge_cap.ppm")
PNG = os.path.join(BUILD, "edge_cap.png")

import importlib.util
spec = importlib.util.spec_from_file_location(
    "ppm_to_png", os.path.join(PROJ, "tools", "ppm_to_png.py"))
ppmmod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ppmmod)


def build_cmd():
    return [
        QEMU, "-machine", "pc", "-m", "2048",
        "-accel", "tcg",
        "-drive", f"file={DISK},format=raw,if=ide",
        "-serial", f"file:{SERIAL}",
        "-monitor", f"tcp:127.0.0.1:{MON_PORT},server,nowait",
        "-display", "none", "-vga", "std",
    ]


def kill_stale():
    try:
        subprocess.run(["taskkill", "/F", "/IM", "qemu-system-x86_64.exe"],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except Exception:
        pass
    time.sleep(1.0)


def send_mon(cmd, retries=4):
    for _ in range(retries):
        try:
            s = socket.create_connection(("127.0.0.1", MON_PORT), timeout=5)
            s.sendall((cmd + "\n").encode())
            time.sleep(0.4)
            s.close()
            return True
        except Exception:
            time.sleep(1.0)
    return False


def wait_mon(timeout=30):
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            s = socket.create_connection(("127.0.0.1", MON_PORT), timeout=2)
            s.close()
            return True
        except Exception:
            time.sleep(0.5)
    return False


def lum(r, g, b):
    return 0.299 * r + 0.587 * g + 0.114 * b


def find_window(w, h, px):
    """Return (x0,y0,x1,y1) bbox of the largest connected region of window-body
    pixels (neutral light ~243).  None if not found."""
    def get(x, y):
        i = (y * w + x) * 3
        return px[i], px[i + 1], px[i + 2]

    def is_body(r, g, b):
        return 238 <= r <= 250 and abs(r - g) <= 5 and abs(r - b) <= 5

    step = 2  # subsample for speed
    best = None
    best_area = 0
    seen = set()
    for yy in range(0, h, step):
        for xx in range(0, w, step):
            if (xx, yy) in seen:
                continue
            r, g, b = get(xx, yy)
            if not is_body(r, g, b):
                continue
            # flood fill (BFS) over step grid
            stack = [(xx, yy)]
            seen.add((xx, yy))
            cnt = 0
            minx = miny = 1 << 30
            maxx = maxy = -1
            while stack:
                cx, cy = stack.pop()
                cnt += 1
                minx = min(minx, cx); maxx = max(maxx, cx)
                miny = min(miny, cy); maxy = max(maxy, cy)
                for dx, dy in ((-step, 0), (step, 0), (0, -step), (0, step)):
                    nx, ny = cx + dx, cy + dy
                    if 0 <= nx < w and 0 <= ny < h and (nx, ny) not in seen:
                        nr, ng, nb = get(nx, ny)
                        if is_body(nr, ng, nb):
                            seen.add((nx, ny))
                            stack.append((nx, ny))
            if cnt > best_area:
                best_area = cnt
                best = (minx, miny, maxx, maxy)
    if best:
        # expand bbox to full-pixel window extent (subsample step) + margin
        x0, y0, x1, y1 = best
        return max(x0 - 2, 0), max(y0 - 2, 0), min(x1 + 2, w - 1), min(y1 + 2, h - 1)
    return None


def main():
    kill_stale()
    with open(SERIAL, "w"):
        pass
    proc = subprocess.Popen(build_cmd(),
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if not wait_mon(30):
        print("[ERR] monitor never came up")
        proc.kill()
        return
    if proc.poll() is not None:
        print(f"[ERR] QEMU exited early rc={proc.returncode}")
        return

    print("[BOOT] waiting for GUI + window...")
    time.sleep(18)
    if not send_mon(f"screendump {PPM}"):
        print("[ERR] screendump failed")
        send_mon("quit"); proc.kill(); return
    time.sleep(1.2)
    if not os.path.exists(PPM) or os.path.getsize(PPM) == 0:
        print("[ERR] no PPM produced")
        send_mon("quit"); proc.kill(); return

    w, h, px = ppmmod.read_ppm(PPM)
    ppmmod.write_png(PNG, w, h, px)
    print(f"[CAP] {w}x{h} -> {PNG}")

    def get(x, y):
        if x < 0 or x >= w or y < 0 or y >= h:
            return None
        i = (y * w + x) * 3
        return px[i], px[i + 1], px[i + 2]

    box = find_window(w, h, px)
    if box is None:
        print("[ERR] window body not found (did the demo window open?)")
        send_mon("quit"); proc.kill(); return
    x0, y0, x1, y1 = box
    print(f"[WINDOW] body bbox x={x0}..{x1} y={y0}..{y1} (w={x1-x0}, h={y1-y0})")

    mid = (y0 + y1) // 2

    # body light + neutral
    body = [get(x, mid) for x in range(x0 + 10, x1 - 10)]
    body = [c for c in body if c]
    body_lum = sum(lum(*c) for c in body) / max(len(body), 1)

    # shadow gradient on the LEFT side: distance 4/8/12/40 px outside the frame
    def avg(d):
        cs = [get(x0 - d + i, mid) for i in range(4)]
        cs = [c for c in cs if c]
        return sum(lum(*c) for c in cs) / max(len(cs), 1) if cs else None

    g4, g8, g12, g40 = avg(4), avg(8), avg(12), avg(40)

    # blue check in the rim band (5px just outside both side frames):
    # a saturated blue *band* was the old rejected look (B-R ~181 over 5px);
    # Win11 allows at most a faint 1px accent hairline (B-R ~50-60).
    blue = []
    for x in list(range(x0 - 5, x0 + 5)) + list(range(x1 - 5, x1 + 5)):
        c = get(x, mid)
        if c:
            blue.append(c[2] - c[0])
    max_blue = max(blue) if blue else 0
    sat_blue = sum(1 for v in blue if v > 100)  # saturated-blue pixels in band

    print(f"[ANALYZE] body_lum = {body_lum:6.1f}")
    print(f"[ANALYZE] left shadow lum @ d=4/8/12/40: "
          f"{'%5.1f'%g4 if g4 else '--'} / {'%5.1f'%g8 if g8 else '--'} / "
          f"{'%5.1f'%g12 if g12 else '--'} / {'%5.1f'%g40 if g40 else '--'}")
    print(f"[ANALYZE] max blue-delta in rim = {max_blue} "
          f"(saturated-blue px={sat_blue}, old glow was ~181 over a 5px band)")

    ok_body = body_lum > 200
    ok_grad = (g4 is not None and g8 is not None and g12 is not None and g40 is not None
               and g4 < g8 - 3 and g8 < g12 - 3 and g12 < g40 - 4)
    ok_noblue = max_blue < 100 and sat_blue <= 4  # at most a 1px faint hairline
    print(f"[VERDICT] body light={ok_body}  soft-shadow gradient={ok_grad}  no blue glow={ok_noblue}")
    print("[VERDICT]", "PASS" if (ok_body and ok_grad and ok_noblue) else "CHECK")

    send_mon("quit")
    try:
        proc.wait(timeout=15)
    except Exception:
        proc.kill()
    print("[DONE]")


if __name__ == "__main__":
    main()
