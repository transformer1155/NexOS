#!/usr/bin/env python3
"""Manual controlled desktop capture for a given os.img.

Starts QEMU with a TCP monitor, waits for splash, types admin+ret,
screendumps, converts ppm->png, prints pixel verdicts.  Same QEMU flags as
capture_desk.py but takes the disk image as argv[1] so we can A/B a backup
disk against the current one.
"""
import os, sys, time, socket, subprocess, importlib.util

PROJ = r"D:\MyOS\bootloader"
BUILD = os.path.join(PROJ, "build")
QEMU = r"D:\qemu\qemu-system-x86_64.exe"
DISK = sys.argv[1] if len(sys.argv) > 1 else os.path.join(BUILD, "os.img")
SERIAL = os.path.join(BUILD, "_serial_manual.txt")
MON_PORT = 4582
PPM = os.path.join(BUILD, "_cap_manual.ppm")
PNG = os.path.join(BUILD, "_cap_manual.png")

spec = importlib.util.spec_from_file_location(
    "ppm_to_png", os.path.join(PROJ, "tools", "ppm_to_png.py"))
ppmmod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ppmmod)


def kill_stale():
    subprocess.run(["taskkill", "/F", "/IM", "qemu-system-x86_64.exe"],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(1.5)


def mon(cmd, retries=8, wait=0.4):
    for _ in range(retries):
        try:
            s = socket.create_connection(("127.0.0.1", MON_PORT), timeout=5)
            s.sendall((cmd + "\n").encode())
            time.sleep(wait)
            s.close()
            return True
        except Exception:
            time.sleep(0.6)
    return False


def main():
    for f in (SERIAL, PPM, PNG):
        try: os.remove(f)
        except FileNotFoundError: pass

    kill_stale()
    cmd = [
        QEMU, "-machine", "pc", "-m", "256",
        "-accel", "tcg,tb-size=128",
        "-drive", f"file={DISK},format=raw,if=ide",
        "-serial", f"file:{SERIAL}",
        "-monitor", f"tcp:127.0.0.1:{MON_PORT},server,nowait",
        "-display", "none", "-vga", "std",
    ]
    print(f"[BOOT] disk={DISK}")
    p = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    deadline = time.time() + 35
    splash = False
    while time.time() < deadline:
        try:
            with open(SERIAL, "r", errors="ignore") as f:
                if "SPLASH] frame drawn" in f.read():
                    splash = True; break
        except FileNotFoundError:
            pass
        time.sleep(0.6)
    if not splash:
        print("[BOOT] WARNING: splash not seen in 35s; proceeding anyway")
    else:
        print("[BOOT] splash detected, waiting 3s for desktop to settle...")
        time.sleep(3.0)

    if not mon("sendkey admin", retries=10):
        print("[MON] sendkey admin failed")
    time.sleep(0.6)
    if not mon("sendkey ret", retries=10):
        print("[MON] sendkey ret failed")

    deadline = time.time() + 20
    rendered = False
    while time.time() < deadline:
        try:
            with open(SERIAL, "r", errors="ignore") as f:
                txt = f.read()
            if "render_all" in txt or "mforms: shell.mex ready" in txt:
                rendered = True; break
        except FileNotFoundError:
            pass
        time.sleep(0.6)
    if rendered:
        print("[DESK] shell.mex ready marker seen")
    else:
        print("[DESK] no ready marker; fixed wait 6s after login")
        time.sleep(6.0)

    time.sleep(2.5)

    if not mon(f"screendump {PPM}", retries=6, wait=0.8):
        print("[MON] screendump failed")

    time.sleep(1.0)
    kill_stale()
    time.sleep(0.5)

    if not os.path.exists(PPM):
        print("[CAP] no ppm produced"); sys.exit(1)
    w, h, px = ppmmod.read_ppm(PPM)
    ppmmod.write_png(PNG, w, h, px)
    sz = os.path.getsize(PNG)
    print(f"[CAP] wrote {PNG} ({sz} bytes)")

    from PIL import Image
    im = Image.open(PNG).convert("RGB")
    w, h = im.size
    px = im.load()
    def lum(x, y):
        r, g, b = px[x, y]
        return 0.2126*r + 0.7152*g + 0.0722*b
    mx = 0
    for y in range(0, h, 8):
        for x in range(0, w, 8):
            mx = max(mx, lum(x, y))
    tb_lums = [lum(x, h-6) for x in range(0, w, 80)]
    cn = lum(w//2, h//2)
    print(f"[SAMPLE] max_lum={mx:.1f}  taskbar_row={[round(v,1) for v in tb_lums[:6]]}  center={cn:.1f}  size={w}x{h}")
    if mx < 5:
        print("[VERDICT] BLACK FRAME")
    elif tb_lums[0] < 5 and cn < 5:
        print("[VERDICT] MOSTLY BLACK")
    else:
        print("[VERDICT] DESKTOP PAINTED (non-black content present)")


if __name__ == "__main__":
    main()
