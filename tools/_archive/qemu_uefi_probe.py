#!/usr/bin/env python3
"""
qemu_uefi_probe.py - Interactive-ish HMP probe for the UEFI GUI black-screen
investigation.  Unlike qemu_uefi_capture.py this one READS the monitor replies,
so QEMU's own error messages ("no such device", "unknown console") are visible,
and it dumps guest PHYSICAL memory at the GOP framebuffer address with `xp`.

Why this matters: the kernel's own read-back of the LFB shows the painted pixel
(lfb[0]=0x005ABEEF), but `screendump` of the ramfb console shows 100% black.
Exactly one of these is looking at the wrong place, and `xp` (QEMU's view of
guest RAM) settles it:
  * xp shows 005ABEEF  -> RAM has the pixels; the display device scans a
                          DIFFERENT address (or the console we dump is not the
                          one GOP handed us).
  * xp shows 00000000  -> the CPU's write never reached RAM (paging/cache/
                          wrong physical address behind an identity map).

Usage:  python3 qemu_uefi_probe.py [wait_s]
"""
import os
import socket
import subprocess
import sys
import time

PROJ = r"D:\MyOS\bootloader"
BUILD = os.path.join(PROJ, "build")
QEMU = r"D:\qemu\qemu-system-x86_64.exe"
OVMF_CODE = r"D:\qemu\share\edk2-x86_64-code.fd"
OVMF_VARS = os.path.join(BUILD, "ovmf_vars_test.fd")
DISK = os.path.join(BUILD, "os_uefi_test.img")
SERIAL = os.path.join(BUILD, "serial_probe.txt")
MON_PORT = 5556


def cmd():
    return [
        QEMU,
        "-machine", "q35", "-m", "2048",
        "-accel", "tcg",
        "-drive", f"if=pflash,format=raw,readonly=on,file={OVMF_CODE}",
        "-drive", f"if=pflash,format=raw,file={OVMF_VARS}",
        "-drive", f"file={DISK},format=raw,if=ide",
        "-serial", f"file:{SERIAL}",
        "-monitor", f"tcp:127.0.0.1:{MON_PORT},server,nowait",
        "-display", "none",
        "-vga", "none",
        "-device", "VGA,id=stdvga",
        "-device", "ramfb,id=rfb",
    ]


class Mon:
    def __init__(self, port, timeout=40):
        t0 = time.time()
        self.s = None
        while time.time() - t0 < timeout:
            try:
                self.s = socket.create_connection(("127.0.0.1", port), timeout=3)
                break
            except Exception:
                time.sleep(0.5)
        if not self.s:
            raise RuntimeError("monitor never came up")
        self.s.settimeout(3)
        self._drain()

    def _drain(self):
        buf = b""
        try:
            while True:
                d = self.s.recv(65536)
                if not d:
                    break
                buf += d
        except Exception:
            pass
        return buf.decode(errors="replace")

    def do(self, c):
        self.s.sendall((c + "\n").encode())
        time.sleep(1.2)
        out = self._drain()
        # strip echo + prompt noise
        lines = [l for l in out.splitlines()
                 if l.strip() and not l.strip().startswith("(qemu)")
                 and l.strip() != c]
        print(f"  $ {c}")
        for l in lines[:14]:
            print(f"      {l}")
        return "\n".join(lines)


def main():
    wait_s = int(sys.argv[1]) if len(sys.argv) > 1 else 45
    with open(SERIAL, "w"):
        pass
    p = subprocess.Popen(cmd(), stdout=subprocess.DEVNULL,
                         stderr=subprocess.DEVNULL)
    try:
        m = Mon(MON_PORT)
        print(f"[PROBE] monitor up; letting the guest boot {wait_s}s ...")
        time.sleep(wait_s)

        print("[PROBE] --- what display devices/consoles exist? ---")
        m.do("info qtree")

        print("[PROBE] --- GOP framebuffer as QEMU sees guest RAM ---")
        # 0x7E4CF000 is the FrameBufferBase the kernel logged via [DGOP]
        m.do("xp /8xw 0x7E4CF000")
        m.do("xp /8xw 0x7E4D0000")

        print("[PROBE] --- screendump each console explicitly ---")
        for dev in ("rfb", "stdvga"):
            f = os.path.join(BUILD, f"probe_{dev}.ppm")
            m.do(f"screendump {f} -d {dev}")
        f = os.path.join(BUILD, "probe_default.ppm")
        m.do(f"screendump {f}")

        m.do("quit")
    finally:
        try:
            p.wait(timeout=10)
        except Exception:
            p.kill()

    print("\n[PROBE] resulting PPMs:")
    for n in ("probe_rfb", "probe_stdvga", "probe_default"):
        fp = os.path.join(BUILD, n + ".ppm")
        if not os.path.exists(fp):
            print(f"  {n}: MISSING")
            continue
        d = open(fp, "rb").read()
        hdr = d.split(b"\n", 3)
        body = d[len(hdr[0]) + len(hdr[1]) + len(hdr[2]) + 3:]
        nz = sum(1 for i in range(0, len(body), 3) if body[i:i+3] != b"\x00\x00\x00")
        tot = len(body) // 3
        print(f"  {n}: {hdr[1].decode()} nonblack={nz}/{tot} "
              f"({100.0*nz/max(tot,1):.2f}%)")


if __name__ == "__main__":
    main()
