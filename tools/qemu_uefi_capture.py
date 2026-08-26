#!/usr/bin/env python3
"""
qemu_uefi_capture.py - Boot the NexOS UEFI GPT disk in QEMU, capture the
GOP framebuffer at intervals (screendump), log the serial port, and
analyze each capture for non-black pixels.

Usage:
    python3 qemu_uefi_capture.py [accel] [captures] [interval_s]

Default: accel auto (whpx then tcg), 8 captures, 12s apart.
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
DISK = os.path.join(BUILD, "os_uefi_test.img")
SERIAL = os.path.join(BUILD, "serial_uefi.txt")
MON_PORT = 5555


def build_cmd(accel):
    cmd = [
        QEMU,
        "-machine", "q35", "-m", "2048",
        "-accel", accel + (",kernel-irqchip=off" if accel == "whpx" else ""),
        "-drive", f"if=pflash,format=raw,readonly=on,file={OVMF_CODE}",
        "-drive", f"if=pflash,format=raw,file={OVMF_VARS}",
        "-drive", f"file={DISK},format=raw,if=ide",
        "-serial", f"file:{SERIAL}",
        "-monitor", f"tcp:127.0.0.1:{MON_PORT},server,nowait",
        "-display", "none",
        # ramfb ONLY -- deliberately no std VGA.
        #
        # THIS IS A CORRECTNESS REQUIREMENT, NOT A PREFERENCE.  QEMU 11.1's HMP
        # `screendump` has NO -d/device option (it errors with "unsupported
        # option -d"), so it can only ever dump console 0.  With the default
        # q35 std VGA present, console 0 is the VGA surface while OVMF's GOP
        # hands the kernel the *ramfb* framebuffer -- every capture then shows
        # a pristine black VGA surface no matter how perfectly the GUI paints,
        # which is exactly the false "black screen" this harness reported for
        # several rounds.  With `-vga none` + ramfb only, console 0 IS the GOP
        # surface, so a plain `screendump` captures what the kernel drew.
        "-vga", "none",
        "-device", "ramfb,id=rfb",
    ]
    return cmd


def send_mon(cmd_str, retries=1):
    """Send one HMP command.  Retries because the monitor socket is not
    listening yet during the first seconds of a cold OVMF boot."""
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


def wait_for_monitor(timeout=30):
    """Block until the HMP socket accepts connections."""
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


def run(accel, n_cap, interval):
    print(f"[QEMU] accel={accel} captures={n_cap} interval={interval}s")
    # fresh serial log (truncate in place; avoid delete so it doesn't trip
    # the host's safe-delete / trash guard on build artifacts)
    with open(SERIAL, "w") as _f:
        pass
    proc = subprocess.Popen(build_cmd(accel),
                            stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
    t_boot = time.time()
    # wait for monitor to come up
    wait_for_monitor(30)
    if proc.poll() is not None:
        print(f"[QEMU] process exited early (rc={proc.returncode}) with accel={accel}")
        return False

    # load the PPM analyzer once
    import importlib.util
    spec = importlib.util.spec_from_file_location(
        "ppm_to_png", os.path.join(PROJ, "tools", "ppm_to_png.py"))
    ppmmod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(ppmmod)

    def grab(dev, ppm, png, tag):
        """screendump console 0, convert to PNG, return non-black %.

        `dev` is kept only for the log label: HMP screendump cannot target a
        console in QEMU 11.1, which is why the VM runs with ramfb as the ONLY
        display device.

        Stale-file guard: a leftover PPM from a previous run made this harness
        silently report the PREVIOUS boot's pixels (two runs printed identical
        0.34%/4.52% figures that way).  We truncate the target first and refuse
        to trust a file QEMU did not just rewrite.
        """
        try:
            with open(ppm, "wb"):
                pass          # truncate to 0 bytes; a real dump refills it
        except Exception:
            pass
        if not send_mon(f"screendump {ppm}", retries=3):
            return None
        time.sleep(1.0)
        if not os.path.exists(ppm) or os.path.getsize(ppm) == 0:
            print(f"    {tag}: no PPM produced (monitor rejected the command?)")
            return None
        try:
            w, h, px = ppmmod.read_ppm(ppm)
            an = ppmmod.analyze(px, w, h)
            ratio = an[2] if isinstance(an, tuple) else an
            ppmmod.write_png(png, w, h, px)
            print(f"    {tag}: {ratio:6.2f}% non-black ({w}x{h}) -> "
                  f"{os.path.basename(png)}")
            return ratio
        except Exception as e:
            print(f"    {tag}: convert failed: {e}")
            return None

    caps = []
    for i in range(n_cap):
        t0 = time.time()
        n = i + 1
        print(f"[CAP {n}] t={int(time.time()-t_boot)}s")
        # console 0 == ramfb == the GOP surface the kernel paints into
        r_gop = grab("rfb",
                     os.path.join(BUILD, f"uefi_cap{n}.ppm"),
                     os.path.join(BUILD, f"uefi_cap{n}.png"), "GOP/ramfb")
        caps.append((n, r_gop, None))
        # wait remainder of interval
        elapsed = time.time() - t0
        if elapsed < interval:
            time.sleep(interval - elapsed)

    best = max((c[1] for c in caps if c[1] is not None), default=0.0)
    print(f"[RESULT] best GOP/ramfb non-black = {best:.2f}%  "
          f"({'GUI VISIBLE' if best > 1.0 else 'STILL BLACK'})")

    # quit
    send_mon("quit")
    try:
        proc.wait(timeout=15)
    except Exception:
        proc.kill()
    print(f"[QEMU] done (accel={accel})")
    return True


def main():
    accel_arg = sys.argv[1] if len(sys.argv) > 1 else "auto"
    n_cap = int(sys.argv[2]) if len(sys.argv) > 2 else 8
    interval = int(sys.argv[3]) if len(sys.argv) > 3 else 12

    accels = ["whpx", "tcg"] if accel_arg == "auto" else [accel_arg]
    for a in accels:
        ok = run(a, n_cap, interval)
        if ok:
            break
        else:
            print(f"[QEMU] retrying with next accel")
    print("[DONE] captures in build/uefi_cap*.png ; serial in build/serial_uefi.txt")


if __name__ == "__main__":
    main()
