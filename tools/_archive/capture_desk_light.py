#!/usr/bin/env python3
"""Headless REAL-VM (os.img) capture + LIGHT-THEME verification.

Same boot/sign-in as capture_desk.py, but here the build was temporarily
flipped to Theme.Dark=0 so we can prove the LIGHT Win11 theme renders on
the real VM (taskbar becomes bright neutral grey ~0xF3F3F3 instead of the
dark 0x202020).  The rendering path is identical to the Settings toggle.
"""
import os, sys, time, socket, subprocess

PROJ = r"D:\MyOS\bootloader"
BUILD = os.path.join(PROJ, "build")
QEMU = r"D:\qemu\qemu-system-x86_64.exe"
DISK = os.path.join(BUILD, "os.img")
SERIAL = os.path.join(BUILD, "serial_desk_light.txt")
MON_PORT = 4609
PPM = os.path.join(BUILD, "desk_cap_light.ppm")
PNG = os.path.join(BUILD, "desk_cap_light.png")

import importlib.util
spec = importlib.util.spec_from_file_location(
    "ppm_to_png", os.path.join(PROJ, "tools", "ppm_to_png.py"))
ppmmod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ppmmod)


def build_cmd():
    return [
        QEMU, "-machine", "pc", "-m", "256",
        "-accel", "tcg,tb-size=128",
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


def mon(cmd, retries=4):
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


def send_keys(text):
    for ch in text:
        mon(f"sendkey {ch}")
        time.sleep(0.12)
    mon("sendkey ret")
    time.sleep(0.3)


def lum(r, g, b):
    return 0.299 * r + 0.587 * g + 0.114 * b


def main():
    kill_stale()
    open(SERIAL, "w").close()
    qlog = open(os.path.join(BUILD, "qemu_light.log"), "w")
    proc = subprocess.Popen(build_cmd(), stdout=qlog, stderr=qlog)
    if not wait_mon(45):
        print("[ERR] monitor never came up")
        try:
            print("--- qemu log ---")
            print(open(os.path.join(BUILD, "qemu_light.log")).read()[:800])
        except Exception:
            pass
        proc.kill()
        return
    if proc.poll() is not None:
        print(f"[ERR] QEMU exited early rc={proc.returncode}")
        return
    time.sleep(40)
    print("[AUTH] typing 'admin' + Enter (root/admin) ...")
    send_keys("admin")
    print("[AUTH] waiting for desktop (LIGHT theme) ...")
    time.sleep(8)

    if not mon(f"screendump {PPM}"):
        print("[ERR] screendump failed")
        mon("quit"); proc.kill(); return
    time.sleep(1.2)
    if not os.path.exists(PPM) or os.path.getsize(PPM) == 0:
        print("[ERR] no PPM produced")
        mon("quit"); proc.kill(); return

    w, h, px = ppmmod.read_ppm(PPM)
    ppmmod.write_png(PNG, w, h, px)
    print(f"[CAP] {w}x{h} -> {PNG}")

    def get(x, y):
        if x < 0 or x >= w or y < 0 or y >= h:
            return None
        i = (y * w + x) * 3
        return px[i], px[i + 1], px[i + 2]

    task = get(int(w * 0.50), h - 3)
    trow = [get(int(w * f), h - 3) for f in (0.1, 0.3, 0.5, 0.7, 0.9)]
    trow = [c for c in trow if c]
    t_lums = [lum(*c) for c in trow]
    t_uni = (max(t_lums) - min(t_lums)) if t_lums else 0
    center = get(int(w * 0.50), int(h * 0.45))

    # locate the bright neutral taskbar band from the bottom up
    band_top = h
    for y in range(h - 1, int(h * 0.80), -1):
        body = 0; total = 0; brs = []
        for x in range(int(0.04 * w), int(0.96 * w)):
            c = get(x, y)
            if c is None: continue
            L = lum(*c); total += 1
            if 160 <= L <= 255: body += 1
            brs.append(c[2] - c[0])
        frac = body / total if total else 0
        med_br = sorted(brs)[len(brs) // 2] if brs else 0
        if frac >= 0.55 and abs(med_br) <= 20:
            band_top = y
        else:
            if band_top < h:
                break
    band_h = h - band_top

    print(f"[SAMPLE] taskbar  = {task}   (expect ~ (243,243,243) light grey)")
    print(f"[SAMPLE] taskbar row lum = {[round(x,1) for x in t_lums]}  spread={t_uni:.1f}")
    print(f"[SAMPLE] light taskbar band y={band_top}..{h}  height={band_h}")
    print(f"[SAMPLE] center   = {center}  (expect bright wallpaper, not dark card)")

    ok_taskbar = (task is not None and lum(*task) >= 180
                  and abs(task[2] - task[0]) <= 20 and t_uni <= 40)
    ok_height = 22 <= band_h <= 90
    ok_no_card = center is not None and lum(*center) > 40

    print(f"[VERDICT] light taskbar band  = {ok_taskbar}  (lum={lum(*task):.1f})")
    print(f"[VERDICT] taskbar height ok   = {ok_height}")
    print(f"[VERDICT] no login card       = {ok_no_card}")
    verdict = ok_taskbar and ok_height and ok_no_card
    print("[VERDICT]", "PASS (LIGHT Win11 theme confirmed on real VM)" if verdict else "CHECK")

    mon("quit")
    try:
        proc.wait(timeout=15)
    except Exception:
        proc.kill()
    print("[DONE]")


if __name__ == "__main__":
    main()
