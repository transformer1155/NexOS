#!/usr/bin/env python3
"""64-bit right-click menu regression test.

Mirrors tools/test_rclick.py but exercises the LONG-MODE kernel:
  1. boot os.img (32-bit stage2/kernel32)
  2. log in (root/admin) so we reach the 32-bit shell
  3. run `switch64` -> 32-bit loader jumps into kernel64.bin (long mode)
  4. enter the GUI (`gui`) on the 64-bit kernel
  5. right-click the wallpaper / taskbar / tray / icon and assert a
     context menu renders (pixel-diff) and the kernel does not fault.

This proves the P0 (right-button edge dispatch in kernel64.cpp mouse loop)
and P1 (gui_cb_mkdir/remove/rename/exec_command wired into the callback
table) work end-to-end under long mode.
"""
import os, sys, socket, time, subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)

IMG = sys.argv[1] if len(sys.argv) > 1 else "build/os.img"
WORK = "build/rclick64_work.img"
LOG = "build/serial_rclick64.log"
BASE = "build/rclick64_base.ppm"
MENU = "build/rclick64_menu.ppm"
PNG = "build/rclick64_menu.png"
PORT = 4471

LIGHT_SUM = 620
DIFF_THRESHOLD = 8000


def mark(s):
    try:
        with open("build/rclick64_result.txt", "a") as f:
            f.write(s + "\n")
    except Exception:
        pass


def wait_sock(port, timeout=30.0):
    end = time.time() + timeout
    while time.time() < end:
        try:
            return socket.create_connection(("127.0.0.1", port), timeout=0.5)
        except OSError:
            time.sleep(0.2)
    raise RuntimeError("monitor not ready")


def type_line(mon, s):
    keymap = {' ': 'spc', '.': 'dot', '/': 'slash', '\\': 'backslash',
              '-': 'minus', '_': 'shift-minus'}
    for ch in s:
        key = f"shift-{ch.lower()}" if 'A' <= ch <= 'Z' else keymap.get(ch, ch)
        mon.sendall(f"sendkey {key}\n".encode())
        time.sleep(0.08)
    mon.sendall(b"sendkey ret\n")
    time.sleep(0.4)


def read_ppm(path):
    with open(path, "rb") as f:
        assert f.readline().strip() == b"P6"
        w, h = (int(x) for x in f.readline().split())
        f.readline()
        px = f.read()
    return w, h, px


def read_ppm_retry(path, tries=10):
    for _ in range(tries):
        try:
            w, h, px = read_ppm(path)
            if len(px) >= w * h * 3:
                return w, h, px
        except (IndexError, AssertionError):
            pass
        time.sleep(0.5)
    return read_ppm(path)


def read_dims(path):
    w, h, _ = read_ppm(path)
    return w, h


def count_light(path):
    w, h, px = read_ppm_retry(path)
    n = 0
    for i in range(0, w * h * 3, 3):
        if px[i] + px[i + 1] + px[i + 2] > LIGHT_SUM:
            n += 1
    return n


def region_pixel_diff(path_a, path_b, x0, y0, rw, rh, tol=24):
    w, h, pa = read_ppm_retry(path_a)
    _, _, pb = read_ppm_retry(path_b)
    x0 = max(0, min(x0, w - 1)); y0 = max(0, min(y0, h - 1))
    x1 = min(w, x0 + rw); y1 = min(h, y0 + rh)
    n = 0
    for yy in range(y0, y1):
        row = yy * w * 3
        for xx in range(x0, x1):
            i = row + xx * 3
            if (abs(pa[i] - pb[i]) + abs(pa[i + 1] - pb[i + 1])
                    + abs(pa[i + 2] - pb[i + 2])) > tol:
                n += 1
    return n


# QEMU monitor mouse_move is RELATIVE; track the real cursor position so
# repeated moves land where intended (see 32-bit test_rclick.py).
_CURSOR = [640, 360]


def mouse_abs(mon, x, y):
    dx, dy = x - _CURSOR[0], y - _CURSOR[1]
    while dx or dy:
        sx = max(-80, min(80, dx)); sy = max(-80, min(80, dy))
        mon.sendall(("mouse_move %d %d\n" % (sx, sy)).encode())
        time.sleep(0.1)
        _CURSOR[0] += sx; _CURSOR[1] += sy
        dx -= sx; dy -= sy
    time.sleep(0.2)


def mouse_press(mon, btn):
    mon.sendall(("mouse_button %d\n" % btn).encode())
    time.sleep(0.15)


def right_click(mon, x, y):
    mouse_abs(mon, x, y)
    mouse_press(mon, 2); time.sleep(0.6)
    mouse_press(mon, 0); time.sleep(0.3)


def menu_check(mon, name, tag, x, y, x0, y0, rw, rh, threshold):
    mouse_abs(mon, 640, 400)          # move to empty wallpaper first
    mouse_press(mon, 1); time.sleep(0.05); mouse_press(mon, 0)  # dismiss any prior menu
    time.sleep(0.4)
    mon.sendall(f"screendump build/{tag}_base.ppm\n".encode()); time.sleep(1.0)
    right_click(mon, x, y); time.sleep(0.5)
    mon.sendall(f"screendump build/{tag}_menu.ppm\n".encode()); time.sleep(1.0)
    d = region_pixel_diff(f"build/{tag}_base.ppm", f"build/{tag}_menu.ppm",
                          x0, y0, rw, rh)
    ok = d >= threshold
    print(f"  {name}: changed px={d} -> {'PASS' if ok else 'FAIL'}")
    mark(f"{name}: {'PASS' if ok else 'FAIL'} (changed={d})")
    return ok


def main():
    subprocess.run(["cp", IMG, WORK], check=True)
    for f in (LOG, BASE, MENU, PNG, "build/rclick64_result.txt"):
        if os.path.exists(f):
            os.remove(f)
    errf = open("build/qemu_rclick64.err", "wb")
    qemu = subprocess.Popen([
        "qemu-system-x86_64",
        "-drive", f"format=raw,file={WORK}",
        "-m", "128M",
        "-vga", "std",
        "-display", "none",
        "-no-reboot",
        "-monitor", f"tcp:127.0.0.1:{PORT},server,nowait",
        "-chardev", f"file,id=ser,path={LOG}",
        "-serial", "chardev:ser",
    ], stdout=errf, stderr=errf)
    try:
        mon = wait_sock(PORT); mon.settimeout(3.0)
        try:
            mon.recv(65536)
        except (TimeoutError, socket.timeout, OSError):
            pass
        print("booting 32-bit kernel...")
        time.sleep(8.0)
        mark("boot ok")
        type_line(mon, "root")
        type_line(mon, "admin")
        time.sleep(1.0)
        print("[switch64] jump into long-mode kernel")
        type_line(mon, "switch64")
        time.sleep(10.0)
        mark("k64 ok")
        # kmain64 now enforces its own login prompt (root/admin)
        print("[login] 64-bit security login (root/admin)")
        type_line(mon, "root")
        type_line(mon, "admin")
        time.sleep(1.5)
        mark("k64 login ok")
        print("[gui] entering managed desktop (64-bit)")
        type_line(mon, "gui")
        time.sleep(12.0)
        mark("gui ok")

        mon.sendall(f"screendump {BASE}\n".encode()); time.sleep(1.5)

        print("[mouse] right-click on wallpaper (64-bit)")
        mouse_abs(mon, 640, 400); time.sleep(0.3)
        mon.sendall(b"mouse_button 2\n"); time.sleep(1.2)
        mon.sendall(b"mouse_button 0\n"); time.sleep(1.5)
        mon.sendall(f"screendump {MENU}\n".encode()); time.sleep(1.5)
        mon.sendall(f"screendump {PNG}\n".encode()); time.sleep(0.5)
        # cleanly close the wallpaper menu before the per-region checks
        mouse_abs(mon, 640, 400); time.sleep(0.2)
        mouse_press(mon, 1); time.sleep(0.05); mouse_press(mon, 0); time.sleep(0.6)

        results = []
        results.append(menu_check(mon, "taskbar pin",
                                  "tb_pin", 505, 690, 485, 618, 280, 140, 2500))
        results.append(menu_check(mon, "taskbar empty",
                                  "tb_bar", 900, 690, 820, 618, 280, 140, 2500))
        results.append(menu_check(mon, "tray popup",
                                  "tray", 1115, 695, 1025, 588, 300, 160, 3000))
        # Desktop icon: click the real icon centre (grid cell 92, margin 22,
        # centre offset 46) and measure the popup that drops down from it.
        w2, h2 = read_dims(BASE)
        per = (h2 - 48 - 12 - 22) // 92
        if per < 1:
            per = 1
        ix = 22 + 0 * 92 + 46          # column 0
        iy = 22 + 0 * 92 + 46          # row 0
        results.append(menu_check(mon, "desktop icon",
                                  "icon", ix, iy, max(0, ix - 170), max(0, iy - 10),
                                  340, 430, 6000))

        mon.sendall(b"quit\n"); time.sleep(1.0)
    finally:
        try:
            qemu.wait(timeout=5.0)
        except subprocess.TimeoutExpired:
            qemu.terminate(); qemu.wait(timeout=3.0)
    errf.close()

    if not os.path.exists(LOG):
        print("ERROR: no serial log. QEMU stderr:")
        with open("build/qemu_rclick64.err", "rb") as f:
            print(f.read().decode("latin-1", "ignore")[:2000])
        return 1

    data = open(LOG, "rb").read().decode("latin-1", "ignore")
    print("\n--- serial log tail ---")
    print("\n".join(data.splitlines()[-22:]))

    ok = True
    if "EXCEPTION" in data or "TRIPLE FAULT" in data or "PANIC" in data:
        print("\nFAIL: kernel fault detected")
        ok = False
    # Both kernels log this once their gui.cpp shell comes online.
    if "[MFORMS] managed shell ready" in data:
        print("PASS: managed shell loaded under 64-bit")
    else:
        print("FAIL: managed shell did not come online")
        ok = False
    if "kmain64 entered" in data or "[K64" in data:
        print("PASS: 64-bit kernel (kmain64) ran")
    else:
        print("WARN: no kmain64 marker in serial log")

    if os.path.exists(BASE) and os.path.exists(MENU):
        base_light = count_light(BASE)
        menu_light = count_light(MENU)
        diff = menu_light - base_light
        print(f"light pixels: base={base_light} menu={menu_light} diff={diff}")
        if diff >= DIFF_THRESHOLD:
            print("PASS: a context menu rendered on right-click (64-bit)")
            mark("desktop menu: PASS (delta=%d)" % diff)
        else:
            print("FAIL: no context menu appeared (image unchanged)")
            mark("desktop menu: FAIL (delta=%d)" % diff)
            ok = False

    if len(results) == 4 and not all(results):
        print("\nFAIL: one or more context menus did not open")
        ok = False

    print("\nRESULT:", "PASS" if ok else "FAIL")
    with open("build/rclick64_result.txt", "w") as f:
        f.write("RESULT: " + ("PASS" if ok else "FAIL") + "\n")
    return 0 if ok else 1


if __name__ == "__main__":
    try:
        rc = main()
    except Exception as ex:
        import traceback
        with open("build/rclick64_result.txt", "w") as f:
            f.write("EXCEPTION: " + str(ex) + "\n")
            f.write(traceback.format_exc())
        print("EXCEPTION", ex)
        sys.exit(1)
    sys.exit(rc)
