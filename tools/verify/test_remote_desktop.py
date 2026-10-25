#!/usr/bin/env python3
"""End-to-end test for the NexOS remote desktop: boot the TEXT-BOOT image
with the NE2000 NIC + hostfwd, log in, run `linux mc_launcher` (a guest
Linux ELF32 that renders a Minecraft-style launcher to the framebuffer),
then drive it over HTTP:
  * GET  /screen  -> compressed NXFB framebuffer, must be non-blank
  * POST /input   -> move pointer / click, must change the rendered GUI
Verifies that a Linux desktop app can be displayed and interacted with
remotely.  Must not crash the kernel (no EXCEPTION in the serial log)."""
import os, sys, socket, time, subprocess, struct, urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)

IMG = sys.argv[1] if len(sys.argv) > 1 else "build/os_textboot.img"
WORK = "build/remotedesktop.img"
LOG = "build/serial_remotedesktop.log"
MONPORT = 4452
HTTPPORT = 18080   # host-side forwarded port -> guest :8080

QEMU = r"D:\qemu\qemu-system-x86_64.exe"
if not os.path.exists(QEMU):
    QEMU = "qemu-system-x86_64"


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


def http_get(path, timeout=5.0):
    url = f"http://127.0.0.1:{HTTPPORT}{path}"
    return urllib.request.urlopen(url, timeout=timeout).read()


def http_post(path, timeout=5.0):
    url = f"http://127.0.0.1:{HTTPPORT}{path}"
    req = urllib.request.Request(url, data=b"", method="POST")
    return urllib.request.urlopen(req, timeout=timeout).read()


def decode_nxfb(buf):
    if buf[:4] != b"NXFB":
        raise RuntimeError("bad magic: %r" % buf[:8])
    w = struct.unpack_from("<I", buf, 4)[0]
    h = struct.unpack_from("<I", buf, 8)[0]
    fmt = struct.unpack_from("<I", buf, 12)[0]
    img = bytearray(w * h * 4)
    p = 16
    i = 0
    tot = w * h
    while i < tot:
        c = struct.unpack_from("<H", buf, p)[0]
        p += 2
        if c == 0:
            break
        r = buf[p]; g = buf[p + 1]; b = buf[p + 2]; p += 3
        for _ in range(c):
            if i >= tot:
                break
            img[i*4] = r; img[i*4+1] = g; img[i*4+2] = b; img[i*4+3] = 255
            i += 1
    return w, h, fmt, img


def px(img, w, x, y):
    o = (y * w + x) * 4
    return img[o], img[o+1], img[o+2]


def main():
    if not os.path.exists(IMG):
        print("ERROR: %s not found. Build it first (make textboot)." % IMG)
        return 1
    import shutil
    shutil.copy(IMG, WORK)
    if os.path.exists(LOG):
        try:
            os.remove(LOG)
        except OSError:
            open(LOG, "w").close()  # truncate in place if recycle-bin delete is blocked
    errf = open("build/qemu_remotedesktop.err", "wb")
    qemu = subprocess.Popen([
        QEMU,
        "-machine", "pc",
        "-drive", f"format=raw,file={WORK},if=ide",
        "-m", "256M",
        "-accel", "tcg",
        "-vga", "std",
        "-display", "none",
        "-no-reboot",
        "-monitor", f"tcp:127.0.0.1:{MONPORT},server,nowait",
        "-net", "nic,model=ne2k_isa",
        "-net", f"user,hostfwd=tcp::{HTTPPORT}-:8080",
        "-chardev", f"file,id=ser,path={LOG}",
        "-serial", "chardev:ser",
    ], stdout=errf, stderr=errf)

    fails = []
    try:
        mon = wait_sock(MONPORT)
        mon.settimeout(3.0)
        try:
            mon.recv(65536)
        except (TimeoutError, socket.timeout):
            pass
        time.sleep(8.0)
        type_line(mon, "root")
        type_line(mon, "admin")
        time.sleep(1.0)
        print("[1] launch mc_launcher (guest Linux ELF32 GUI)")
        type_line(mon, "linux mc_launcher")
        time.sleep(2.0)

        # Wait for the guest to announce itself.
        ready = False
        for _ in range(40):
            if os.path.exists(LOG):
                with open(LOG, "rb") as f:
                    if b"MC_LAUNCHER: ready" in f.read():
                        ready = True
                        break
            time.sleep(0.3)
        if not ready:
            fails.append("mc_launcher did not report ready")
            print("FAIL: mc_launcher ready marker missing")

        # Poll /screen until we get a valid frame.
        fb = None
        for _ in range(50):
            try:
                data = http_get("/screen")
                if len(data) > 16:
                    w, h, fmt, img = decode_nxfb(data)
                    fb = (w, h, fmt, img)
                    break
            except Exception:
                pass
            time.sleep(0.3)
        if fb is None:
            fails.append("/screen returned no valid framebuffer")
            print("FAIL: could not fetch /screen")
        else:
            w, h, fmt, img = fb
            print(f"[2] /screen: {w}x{h} fmt={fmt} bytes={len(data)}")
            # non-blank: count distinct coarse colors
            colors = set()
            for y in range(0, h, 17):
                for x in range(0, w, 17):
                    colors.add(px(img, w, x, y))
            if len(colors) < 5:
                fails.append("/screen looks blank (few colors)")
                print("FAIL: /screen appears blank")
            else:
                print(f"    distinct sampled colors: {len(colors)} (OK)")

            # initial PLAY button center
            r0, g0, b0 = px(img, w, 512, 660)
            print(f"[3] initial PLAY center rgb=({r0},{g0},{b0})")
            if not (g0 > r0 and g0 > b0):
                fails.append("initial PLAY button not green")
                print("FAIL: initial PLAY button not green")

            # hover over PLAY
            http_post("/input?mx=512&my=660&mb=0&key=0&down=0")
            time.sleep(0.5)
            w, h, fmt, img = decode_nxfb(http_get("/screen"))
            r1, g1, b1 = px(img, w, 512, 660)
            print(f"[4] hover PLAY center rgb=({r1},{g1},{b1})")
            if not (g1 > r1 and g1 > b1):
                fails.append("hover PLAY button not green")
                print("FAIL: hover PLAY not green")
            # brighter than before (hover green is 102,200,106 vs 76,175,80)
            if g1 <= g0:
                fails.append("hover did not brighten PLAY button")
                print("FAIL: hover did not change PLAY color")

            # click PLAY
            http_post("/input?mx=512&my=660&mb=1&key=0&down=1")
            time.sleep(0.4)
            w, h, fmt, img = decode_nxfb(http_get("/screen"))
            r2, g2, b2 = px(img, w, 512, 660)
            print(f"[5] click PLAY center rgb=({r2},{g2},{b2})")
            if not (r2 > g2 and r2 > b2):
                fails.append("click PLAY did not turn button orange")
                print("FAIL: click PLAY did not turn button orange (launching)")
            else:
                print("    launch state -> orange (OK)")

        # verify HTTP viewer page exists
        try:
            html = http_get("/desktop").decode("latin-1", "ignore")
            if "NXFB" not in html and "canvas" not in html:
                fails.append("/desktop viewer missing")
                print("FAIL: /desktop viewer malformed")
            else:
                print("[6] /desktop viewer served OK")
        except Exception as e:
            fails.append("/desktop unreachable: %s" % e)
            print("FAIL: /desktop unreachable")

        mon.sendall(b"quit\n")
        time.sleep(1.0)
    finally:
        try:
            qemu.wait(timeout=5.0)
        except subprocess.TimeoutExpired:
            qemu.terminate()
            qemu.wait(timeout=3.0)
    errf.close()

    with open(LOG, "rb") as f:
        data = f.read().decode("latin-1", "ignore")
    if "EXCEPTION" in data:
        fails.append("kernel EXCEPTION detected")
        print("\nFAIL: kernel exception in serial log")

    print("\n--- serial log tail ---")
    print("\n".join(data.splitlines()[-25:]))

    if fails:
        print("\nRESULT: FAIL")
        for f in fails:
            print("  - " + f)
        return 1
    print("\nRESULT: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
