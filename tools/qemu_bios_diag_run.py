#!/usr/bin/env python3
"""Boot the BIOS raw image (os.img) in QEMU, capture the VGA framebuffer and
the COM1 serial log.  Used to verify the 64-bit diagnostic kernel (IDT +
diag_step + fault_common) which, on the BIOS path, is loaded from LBA 2048
by the real loader (unlike the UEFI path where it is embedded in BOOTX64.EFI).

Usage: python3 qemu_bios_diag_run.py [accel] [captures] [interval_s] [mem_mb]

mem_mb defaults to 1024.  It used to be a hard-coded 2048, which silently
broke on a host with less than ~2 GiB free: QEMU printed "cannot set up guest
memory 'pc.ram'" and exited, and because stderr was routed to DEVNULL the only
symptom was "exited early rc=1" with no cause.  QEMU's stderr is now kept in
build/qemu_bios_diag.err and echoed on early exit.  The kernel only needs the
low 32 MiB, so 1024 is generous.
"""
import os
import sys
import time
import socket
import subprocess

PROJ = r"D:\MyOS\bootloader"
BUILD = os.path.join(PROJ, "build")
QEMU = r"D:\qemu\qemu-system-x86_64.exe"
DISK = os.path.join(BUILD, "os.img")
SERIAL = os.path.join(BUILD, "serial_diag.txt")
QEMU_ERR = os.path.join(BUILD, "qemu_bios_diag.err")
MON_PORT = 5556


def build_cmd(accel, mem_mb):
    return [
        QEMU,
        "-machine", "pc", "-m", str(mem_mb),
        "-accel", accel + (",kernel-irqchip=off" if accel == "whpx" else ""),
        "-drive", f"file={DISK},format=raw,if=ide",
        "-serial", f"file:{SERIAL}",
        "-monitor", f"tcp:127.0.0.1:{MON_PORT},server,nowait",
        "-display", "none",
        "-vga", "std",
    ]


def send_mon(cmd_str, retries=3):
    for _ in range(retries):
        try:
            s = socket.create_connection(("127.0.0.1", MON_PORT), timeout=5)
            s.sendall((cmd_str + "\n").encode())
            time.sleep(0.5)
            s.close()
            return True
        except Exception:
            time.sleep(1.0)
    return False


def wait_for_monitor(timeout=30):
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            s = socket.create_connection(("127.0.0.1", MON_PORT), timeout=2)
            s.close()
            return True
        except Exception:
            time.sleep(0.5)
    return False


def run(accel, n_cap, interval, mem_mb):
    print(f"[QEMU] accel={accel} captures={n_cap} interval={interval}s mem={mem_mb}M")
    with open(SERIAL, "w"):
        pass
    errf = open(QEMU_ERR, "w")
    proc = subprocess.Popen(build_cmd(accel, mem_mb),
                            stdout=errf, stderr=subprocess.STDOUT)
    wait_for_monitor(30)
    if proc.poll() is not None:
        errf.flush()
        errf.close()
        print(f"[QEMU] exited early rc={proc.returncode}")
        try:
            with open(QEMU_ERR) as f:
                msg = f.read().strip()
            if msg:
                print("[QEMU] stderr:")
                for line in msg.splitlines()[:10]:
                    print("       " + line)
                if "guest memory" in msg:
                    print("[HINT] host is short on RAM -- rerun with a smaller "
                          "mem_mb, e.g. ... tcg 4 8 768")
        except Exception:
            pass
        return False

    import importlib.util
    spec = importlib.util.spec_from_file_location(
        "ppm_to_png", os.path.join(PROJ, "tools", "ppm_to_png.py"))
    ppmmod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(ppmmod)

    def grab(ppm, png, tag):
        try:
            with open(ppm, "wb"):
                pass
        except Exception:
            pass
        if not send_mon(f"screendump {ppm}", retries=3):
            return None
        time.sleep(1.0)
        if not os.path.exists(ppm) or os.path.getsize(ppm) == 0:
            print(f"    {tag}: no PPM")
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
        print(f"[CAP {i+1}] t={int(time.time()-t0)}s")
        r = grab(os.path.join(BUILD, f"bios_cap{i+1}.ppm"),
                 os.path.join(BUILD, f"bios_cap{i+1}.png"), "VGA")
        caps.append(r)
        el = time.time() - t0
        if el < interval:
            time.sleep(interval - el)
    best = max((c for c in caps if c is not None), default=0.0)
    print(f"[RESULT] best VGA non-black = {best:.2f}% "
          f"({'GUI VISIBLE' if best > 1.0 else 'STILL BLACK'})")
    send_mon("quit")
    try:
        proc.wait(timeout=15)
    except Exception:
        proc.kill()
    print("[DONE] captures build/bios_cap*.png ; serial build/serial_diag.txt")
    return True


def main():
    accel = sys.argv[1] if len(sys.argv) > 1 else "tcg"
    n_cap = int(sys.argv[2]) if len(sys.argv) > 2 else 6
    interval = int(sys.argv[3]) if len(sys.argv) > 3 else 10
    mem_mb = int(sys.argv[4]) if len(sys.argv) > 4 else int(
        os.environ.get("QEMU_MEM_MB", "1024"))
    run(accel, n_cap, interval, mem_mb)


if __name__ == "__main__":
    main()
