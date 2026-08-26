#!/usr/bin/env python3
"""
qemu_highfb_test.py - Verify the >4GB GOP framebuffer mapping path that only
real hardware (LFB at e.g. 0x4000000000) exercises.  QEMU's ramfb normally
reports the FB below 4GB, so that code is never reached in normal runs.

We boot a TEST build of BOOTX64.EFI that fakes framebuffer_phys64 = 0x4000000000
(guarded by TEST_HIGH_FB).  0x4000000000 is 256 GiB, so on a normal developer
host QEMU cannot back it with RAM; stores fall into a black hole and the visible
screen stays at the low GOP surface.  That is expected.  What this test proves is
that the kernel constructs the high-FB page tables, maps the faked address, and
reaches the GUI render loop without a page fault / triple fault.  Pixel output
is covered by the normal (low-FB) build.

Memory size is configurable via QEMU_HFB_MEM (default 1G); the fake address is
far beyond guest RAM in any case, so the exact size only affects whether QEMU
can start on the host.

Usage:  python3 qemu_highfb_test.py [wait_s]
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
DISK = os.path.join(BUILD, "os_uefi_highfb_test.img")
SERIAL = os.path.join(BUILD, "serial_highfb.txt")
MON_PORT = 5559


def cmd():
    mem = os.environ.get("QEMU_HFB_MEM", "1G")
    return [
        QEMU,
        "-machine", "q35", "-m", mem,
        "-accel", "tcg",
        "-drive", f"if=pflash,format=raw,readonly=on,file={OVMF_CODE}",
        "-drive", f"if=pflash,format=raw,file={OVMF_VARS}",
        "-drive", f"file={DISK},format=raw,if=ide",
        "-serial", f"file:{SERIAL}",
        "-monitor", f"tcp:127.0.0.1:{MON_PORT},server,nowait",
        "-display", "none",
        "-vga", "none",
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
        lines = [l for l in out.splitlines()
                 if l.strip() and not l.strip().startswith("(qemu)")
                 and l.strip() != c]
        print(f"  $ {c}")
        for l in lines[:16]:
            print(f"      {l}")
        return "\n".join(lines)


def u32(hi, lo):
    return (hi << 32) | lo


def analyze_xp(lines):
    """Parse `xp /Nxw ADDR` output: each line is 1 address + up to 4 u32 words."""
    words = []
    for l in lines:
        parts = l.split()
        if not parts:
            continue
        # first token is the address (ends with ':'); rest are hex words
        for tok in parts[1:]:
            try:
                words.append(int(tok, 16))
            except ValueError:
                pass
    return words


def main():
    wait_s = int(sys.argv[1]) if len(sys.argv) > 1 else 45
    open(SERIAL, "w").close()
    qemu_err = os.path.join(BUILD, "qemu_highfb_err.txt")
    with open(qemu_err, "w") as ef:
        p = subprocess.Popen(cmd(), stdout=subprocess.DEVNULL,
                             stderr=ef)
    try:
        m = Mon(MON_PORT)
        print(f"[HFB] monitor up; booting {wait_s}s for GUI to paint 0x4000000000 ...")
        time.sleep(wait_s)

        print("[HFB] --- guest RAM at the faked high framebuffer 0x4000000000 ---")
        out1 = m.do("xp /256xw 0x4000000000")
        out2 = m.do("xp /64xw 0x4000004000")
        m.do("quit")
    finally:
        try:
            p.wait(timeout=10)
        except Exception:
            p.kill()

    w1 = analyze_xp(out1.splitlines())
    w2 = analyze_xp(out2.splitlines())

    nz = sum(1 for w in w1 if w != 0)
    # Desktop wallpaper is blue-ish (BGRX: high byte 0, R low). Look for the
    # known desktop fill 0x005ABEEF and sky-blue gradient 0x0085C3E7-ish.
    desktop_hits = sum(1 for w in w1 if w in (0x005ABEEF,) or (w & 0xFFFFFF00) in (0x0085C300, 0x005ABE00))
    print(f"\n[HFB] 0x4000000000: {len(w1)} words read, {nz} non-zero")
    print(f"[HFB] desktop-colored words (0x005ABEEF / sky-blue): {desktop_hits}")
    # show a few sample words
    sample = [f"0x{w:08X}" for w in w1[:12]]
    print(f"[HFB] sample: {sample}")

    # Cross-check the serial log: did the kernel log the faked FB + mapping selfcheck?
    print("[HFB] --- serial cross-check ---")
    s = ""
    try:
        s = open(SERIAL, "r", errors="replace").read()
        for key in ("TEST_HIGH_FB", "[DGOP]", "fb64=0x4000000000", "MATCH", "MISMATCH",
                    "High framebuffer", "[FBMAP] high", "Entered GUI mode", "render_all"):
            if key in s:
                # print the matching line(s)
                for ln in s.splitlines():
                    if key in ln:
                        print(f"      serial: {ln.strip()[:120]}")
                        break
        if "fb64=0x4000000000" not in s:
            print("      (NOTE: serial does not show fb64=0x4000000000 -- check [DGOP] line)")
    except Exception as e:
        print(f"      (serial read failed: {e})")

    # The fake address 0x4000000000 (256 GiB) is far beyond any RAM QEMU can
    # provide on a normal host, so the visible display FB stays at the low GOP
    # surface and stores to 0x4000000000 fall into a black hole.  That is
    # expected and intended.  What we CAN verify locally is that the kernel
    # constructed the high-FB page tables, reached the GUI render loop, and did
    # not #PF/triple-fault on the way.
    has_fbmap_high = "[FBMAP] high" in s or "High framebuffer" in s
    has_high_marker = "fb64=0x00000000HIGH" in s or "fb64=0x4000000000" in s
    no_fault = "DIAG-FAULT" not in s and "triple" not in s.lower()
    reached_gui = "Entered GUI mode" in s and "render_all" in s

    if has_fbmap_high and has_high_marker and no_fault and reached_gui:
        print("\n[HFB] RESULT: PASS - high-FB mapping path exercised, GUI reached, no fault")
        print("        (pixels not observable because 0x4000000000 has no backing RAM in QEMU)")
    elif no_fault and reached_gui:
        print("\n[HFB] RESULT: PARTIAL - reached GUI without fault, but high-FB mapping markers missing")
    else:
        print("\n[HFB] RESULT: FAIL - high-FB path did not reach GUI cleanly (see serial above)")


if __name__ == "__main__":
    main()
