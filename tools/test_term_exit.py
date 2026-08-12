#!/usr/bin/env python3
"""Verify the Terminal shortcut exits the GUI on a SINGLE click, both from
   the bottom taskbar pin and the desktop Terminal icon (task 1: terminal
   == drop to text shell; never opens/toggles a window)."""
import os, sys, time, socket, subprocess, threading
from test_desktop_ui import (wait_sock, _ser_reader, log_text, wait_for_log,
                              type_line, click, SER_BUF)

IMG = "build/os.img"
WORK = "build/os_te.img"
MPORT = 4511
SPORT = 4512


def ppm_dims(path):
    with open(path, "rb") as f:
        assert f.readline().strip() == b"P6"
        wh = f.readline().split()
        return int(wh[0]), int(wh[1])


def term_pin_xy(W, H):
    BtnSz, BtnGap, tN = 40, 6, 7
    GroupW = (tN + 1) * BtnSz + tN * BtnGap
    bx = (W - GroupW) // 2
    if bx < 8:
        bx = 8
    i = 1
    x = bx + (i + 1) * (BtnSz + BtnGap) + BtnSz // 2
    y = (H - 48) + (48 - BtnSz) // 2 + BtnSz // 2
    return x, y


def main():
    subprocess.run(["cp", IMG, WORK], check=True)
    errf = open("build/qemu_te.err", "wb")
    qemu = subprocess.Popen([
        "qemu-system-x86_64", "-drive", f"format=raw,file={WORK}",
        "-m", "128M", "-vga", "std", "-display", "none", "-no-reboot",
        "-monitor", f"tcp:127.0.0.1:{MPORT},server,nowait",
        "-serial", f"tcp:127.0.0.1:{SPORT},server,nowait",
    ], stdout=errf, stderr=errf)
    mon = None
    checks = {}
    try:
        mon = wait_sock(MPORT); mon.settimeout(3.0)
        ser = wait_sock(SPORT)
        threading.Thread(target=_ser_reader, args=(ser,), daemon=True).start()
        try:
            mon.recv(65536)
        except (TimeoutError, socket.timeout):
            pass
        if not wait_for_log("[K5] Hello world written", 90.0):
            print("RESULT: FAIL (boot)"); return 1
        time.sleep(2.0)
        for _ in range(3):
            type_line(mon, "root"); time.sleep(0.6)
            type_line(mon, "admin"); time.sleep(1.5)
            type_line(mon, "echo boot-ok")
            if wait_for_log("boot-ok", 20.0):
                break
            mon.sendall(b"sendkey ret\n"); time.sleep(1.5)
        type_line(mon, "gui")
        if not wait_for_log("[GUI] Entered GUI mode", 30.0):
            print("RESULT: FAIL (no gui)"); return 1
        time.sleep(1.5)

        buf0 = len(SER_BUF)

        # --- taskbar Terminal pin: single click must exit ---
        W, H = ppm_dims("build/te_a.ppm") if os.path.exists("build/te_a.ppm") else (1280, 720)
        # grab to learn resolution
        if not os.path.exists("build/te_a.ppm"):
            pass
        mon.sendall(b"screendump build/te_a.ppm\n"); time.sleep(1.0)
        W, H = ppm_dims("build/te_a.ppm")
        tx, ty = term_pin_xy(W, H)
        print(f"resolution {W}x{H}, taskbar Terminal pin ({tx},{ty})")
        click(mon, tx, ty)
        checks["taskbar_single_exit"] = "[GUI] Exited GUI mode" in log_text()[buf0:]
        print("taskbar single click exits:", checks["taskbar_single_exit"])

        # back to GUI for desktop-icon test
        time.sleep(0.5)
        type_line(mon, "gui")
        wait_for_log("[GUI] Entered GUI mode", 30.0)
        time.sleep(1.5)
        buf1 = len(SER_BUF)
        # desktop Terminal icon (index 1) centre ~ (64,156)
        click(mon, 64, 156)
        time.sleep(1.0)
        checks["desktop_single_exit"] = "[GUI] Exited GUI mode" in log_text()[buf1:]
        print("desktop Terminal single click exits:", checks["desktop_single_exit"])

        ok = checks.get("taskbar_single_exit", False) and checks.get("desktop_single_exit", False)
        print("RESULT:", "PASS" if ok else "FAIL")
        for k, v in checks.items():
            print(f"  {k}: {v}")
        return 0 if ok else 1
    finally:
        try:
            if mon: mon.sendall(b"quit\n")
        except Exception: pass
        qemu.terminate()
        try: qemu.wait(timeout=5)
        except Exception: qemu.kill()


if __name__ == "__main__":
    sys.exit(main())
