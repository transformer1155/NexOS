#!/usr/bin/env python3
"""Headless verify of 64-bit kernel vec_init() (X-stage TrueType load).

Forces the 32-bit kernel to switch into the 64-bit kernel in TEXT mode via
the boot_no_gui loader flag (0x501E).  The 64-bit kernel calls gui_init()
unconditionally, which runs vec_init() and prints [VECPEEK]/[VECERR] lines.
We only need those serial lines, so we boot, wait, and dump them.
"""
import os, sys, time, socket, subprocess

PROJ = r"D:\MyOS\bootloader"
BUILD = os.path.join(PROJ, "build")
QEMU = r"D:\qemu\qemu-system-x86_64.exe"
DISK = os.path.join(BUILD, "os.img")
SERIAL = os.path.join(BUILD, "serial_vec.txt")
MON_PORT = 4591


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
            time.sleep(0.3)
            s.close()
            return True
        except Exception:
            time.sleep(1.0)
    return False


def main():
    kill_stale()
    open(SERIAL, "w").close()
    proc = subprocess.Popen(build_cmd(),
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    # wait for monitor
    t0 = time.time()
    while time.time() - t0 < 30:
        try:
            s = socket.create_connection(("127.0.0.1", MON_PORT), timeout=2)
            s.close()
            break
        except Exception:
            time.sleep(0.5)
    else:
        print("[ERR] monitor never up")
        proc.kill()
        return

    # give the 64-bit kernel time to boot and run gui_init
    time.sleep(25)
    if proc.poll() is not None:
        print("[ERR] QEMU exited early", proc.returncode)

    mon("quit")
    try:
        proc.wait(timeout=10)
    except Exception:
        proc.kill()

    # dump VEC lines + any vec-related context
    with open(SERIAL, "rb") as f:
        txt = f.read().decode("utf-8", "ignore")
    for line in txt.splitlines():
        if "VECPEEK" in line or "VECERR" in line or "vec_init" in line \
           or "step=318" in line or "step=319" in line \
           or "msyh" in line.lower() or "shell.mex ready" in line \
           or "K64-6" in line or "SFS mounted" in line:
            print(line)
    print("[DONE]")


if __name__ == "__main__":
    main()
