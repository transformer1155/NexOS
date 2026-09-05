#!/usr/bin/env python3
"""
qemu_uefi_verify_diag.py - Boot the NexOS UEFI GPT disk (os_uefi_diag.img) in
QEMU tcg, capture the GOP/ramfb framebuffer at intervals (screendump), log the
serial port, and report non-black pixel % per frame plus key serial markers.

Forces -accel tcg (WHPX is unavailable on this host) and -m 512 (the 'pc.ram'
guest-memory allocation intermittently fails with larger RAM on this box).

Usage:
    python3 qemu_uefi_verify_diag.py [captures] [interval_s]
Default: 6 captures, 7s apart.
"""
import os
import sys
import time
import socket
import subprocess

PROJ = r"D:\MyOS\bootloader"
BUILD = os.path.join(PROJ, "build")
QEMU = r"D:\qemu\qemu-system-x86_64.exe"
OVMF_CODE = r"D:\qemu\share\edk2-x86_64-code.fd"
OVMF_VARS = os.path.join(BUILD, "ovmf_vars_test.fd")
DISK = os.path.join(BUILD, "os_uefi_diag.img")   # <-- the image we just injected
SERIAL = os.path.join(BUILD, "serial_uefi_diag.txt")
MON_PORT = 5666


def build_cmd():
    # ramfb ONLY: with std VGA present, console 0 is the VGA surface while the
    # kernel paints into the GOP/ramfb surface, so screendump would show black.
    return [
        QEMU,
        "-machine", "q35", "-m", "256",
        # tb-size caps the TCG JIT buffer so we don't exhaust the Windows page
        # file ("allocate ... for jit buffer: page file too small" -> rc=1).
        "-accel", "tcg,tb-size=64",
        "-drive", f"if=pflash,format=raw,readonly=on,file={OVMF_CODE}",
        "-drive", f"if=pflash,format=raw,file={OVMF_VARS}",
        "-drive", f"file={DISK},format=raw,if=ide",
        "-serial", f"file:{SERIAL}",
        "-monitor", f"tcp:127.0.0.1:{MON_PORT},server,nowait",
        "-display", "none",
        "-vga", "none",
        "-device", "ramfb,id=rfb",
    ]


def send_mon(cmd_str, retries=3):
    for attempt in range(retries):
        try:
            s = socket.create_connection(("127.0.0.1", MON_PORT), timeout=5)
            s.sendall((cmd_str + "\n").encode())
            time.sleep(0.5)
            s.close()
            return True
        except Exception as e:
            if attempt + 1 >= retries:
                print(f"[MON] send failed: {e}")
                return False
            time.sleep(1.0)
    return False


def wait_for_monitor(timeout=40):
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            s = socket.create_connection(("127.0.0.1", MON_PORT), timeout=2)
            s.close()
            print(f"[MON] ready after {time.time() - t0:.1f}s")
            return True
        except Exception:
            time.sleep(0.5)
    print("[MON] never became ready")
    return False


def main():
    n_cap = int(sys.argv[1]) if len(sys.argv) > 1 else 6
    interval = int(sys.argv[2]) if len(sys.argv) > 2 else 7

    spec = importlib_spec()
    ppmmod = load_ppm(spec)

    print(f"[QEMU] disk={os.path.basename(DISK)} accel=tcg -m 512 captures={n_cap} interval={interval}s")
    with open(SERIAL, "w"):
        pass
    t_boot = time.time()
    proc = subprocess.Popen(build_cmd(),
                            stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
    if not wait_for_monitor(40):
        print("[QEMU] monitor never ready; aborting")
        proc.kill()
        return
    if proc.poll() is not None:
        print(f"[QEMU] process exited early (rc={proc.returncode})")
        return

    results = []
    for i in range(n_cap):
        t0 = time.time()
        n = i + 1
        ppm = os.path.join(BUILD, f"uefi_diag_cap{n}.ppm")
        png = os.path.join(BUILD, f"uefi_diag_cap{n}.png")
        try:
            with open(ppm, "wb"):
                pass
        except Exception:
            pass
        ok = send_mon(f"screendump {ppm}", retries=3)
        time.sleep(1.0)
        ratio = None
        if ok and os.path.exists(ppm) and os.path.getsize(ppm) > 0:
            try:
                w, h, px = ppmmod.read_ppm(ppm)
                nb, tot, pct = ppmmod.analyze(px, w, h)
                ppmmod.write_png(png, w, h, px)
                ratio = pct
                print(f"[CAP {n}] t={int(time.time()-t_boot)}s: GOP/ramfb {pct:6.2f}% "
                      f"non-black ({w}x{h}) -> {os.path.basename(png)}")
            except Exception as e:
                print(f"[CAP {n}] convert failed: {e}")
        else:
            print(f"[CAP {n}] t={int(time.time()-t_boot)}s: no PPM (monitor rejected?)")
        results.append((n, ratio))
        elapsed = time.time() - t0
        if elapsed < interval:
            time.sleep(interval - elapsed)

    best = max((r for _, r in results if r is not None), default=0.0)
    print(f"[RESULT] best GOP/ramfb non-black = {best:.2f}%  "
          f"({'GUI VISIBLE' if best > 1.0 else 'STILL BLACK'})")

    # dump serial tail for boot markers
    print("[SERIAL] reading", SERIAL)
    try:
        with open(SERIAL, "r", errors="replace") as f:
            lines = f.read().splitlines()
        print(f"[SERIAL] {len(lines)} lines")
        for ln in lines[-50:]:
            print("   | " + ln)
    except Exception as e:
        print(f"[SERIAL] read failed: {e}")

    send_mon("quit")
    try:
        proc.wait(timeout=15)
    except Exception:
        proc.kill()
    print("[QEMU] done")


def importlib_spec():
    import importlib.util
    spec = importlib.util.spec_from_file_location(
        "ppm_to_png", os.path.join(PROJ, "tools", "ppm_to_png.py"))
    return spec


def load_ppm(spec):
    import importlib.util
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


if __name__ == "__main__":
    main()
