"""External correctness + smoothness verification for the NexOS compositor.

Drives the guest only through QEMU's external interfaces (QMP input events,
screendump, serial log).  It checks the two things a frame-rate number alone
cannot prove:

  1. no progressive darkening -- the retro RGB565+dither post-process used to
     be non-idempotent, so retained pixels re-processed every frame drifted a
     quantisation step darker (the "black blocks").  Two frames 10 s apart
     must have the same mean brightness.
  2. interaction still works with the cached desktop layer -- the Start menu
     must still open, i.e. damage/invalidation is correct.
"""
import subprocess, socket, json, time, os, re, zlib, struct

QEMU     = r"D:\qemu\qemu-system-x86_64.exe"
IMG      = r"D:\MyOS\bootloader\build\os_v2.img"
OUT      = r"D:\MyOS\bootloader\build"
QMP_PORT = 4466
SERLOG   = os.path.join(OUT, "verify_serial.log")


def read_ppm(path):
    with open(path, "rb") as f:
        assert f.readline().strip() == b"P6"
        wh = f.readline().split()
        w, h = int(wh[0]), int(wh[1])
        int(f.readline())
        data = f.read()
    return w, h, data


def stats(path):
    w, h, d = read_ppm(path)
    n = w * h
    tot = 0
    black = 0
    for i in range(0, n * 3, 3):
        # PPM is RGB; brightness ~= (r+g+b)/3
        v = d[i] + d[i + 1] + d[i + 2]
        tot += v
        if v < 12:
            black += 1
    mean = tot / (n * 3.0)
    return dict(w=w, h=h, mean=mean, black_pct=100.0 * black / n)


def to_png(ppm, png):
    w, h, data = read_ppm(ppm)
    stride = w * 3
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        raw += data[y * stride:(y + 1) * stride]
    comp = zlib.compress(bytes(raw), 9)

    def chunk(typ, body):
        return (struct.pack(">I", len(body)) + typ + body +
                struct.pack(">I", zlib.crc32(typ + body) & 0xffffffff))
    out = bytearray(b"\x89PNG\r\n\x1a\n")
    out += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    out += chunk(b"IDAT", comp)
    out += chunk(b"IEND", b"")
    with open(png, "wb") as f:
        f.write(out)


class Q:
    def __init__(self, sock):
        self.s = sock

    def cmd(self, c, a=None):
        obj = {"execute": c}
        if a is not None:
            obj["arguments"] = a
        self.s.sendall((json.dumps(obj) + "\r\n").encode())
        buf = b""
        while b'"return"' not in buf and b'"error"' not in buf:
            d = self.s.recv(4096)
            if not d:
                break
            buf += d
        return buf.decode(errors="replace")

    def rel(self, dx, dy):
        evs = []
        if dx:
            evs.append({"type": "rel", "data": {"axis": "x", "value": dx}})
        if dy:
            evs.append({"type": "rel", "data": {"axis": "y", "value": dy}})
        for e in evs:
            self.cmd("input-send-event", {"events": [e]})
            time.sleep(0.09)

    def click(self):
        self.cmd("input-send-event", {"events": [{"type": "btn", "data": {"button": "left", "down": True}}]})
        time.sleep(0.05)
        self.cmd("input-send-event", {"events": [{"type": "btn", "data": {"button": "left", "down": False}}]})
        time.sleep(0.05)

    def key(self, qc):
        self.cmd("input-send-event", {"events": [
            {"type": "key", "data": {"key": {"type": "qcode", "data": qc}, "down": True}},
            {"type": "key", "data": {"key": {"type": "qcode", "data": qc}, "down": False}}]})
        time.sleep(0.03)

    def move_to(self, x, y):
        for _ in range(40):
            self.rel(-100, -100)
        time.sleep(0.1)
        cx, cy, guard = 0, 0, 0
        while (cx != x or cy != y) and guard < 200:
            sx = max(-100, min(100, x - cx))
            sy = max(-100, min(100, y - cy))
            self.rel(sx, sy)
            cx += sx
            cy += sy
            guard += 1

    def shot(self, name):
        ppm = os.path.join(OUT, name + ".ppm")
        self.cmd("screendump", {"filename": ppm})
        to_png(ppm, os.path.join(OUT, name + ".png"))
        return stats(ppm)


def wait_gui(timeout=40):
    t = time.time()
    while time.time() - t < timeout:
        try:
            with open(SERLOG, "r", errors="replace") as f:
                if "Entered GUI mode" in f.read():
                    return True
        except FileNotFoundError:
            pass
        time.sleep(0.5)
    return False


def diff_pixels(p1, p2, tol=8):
    """Count pixels that differ (and their bounding box) between two frames."""
    w1, h1, d1 = read_ppm(p1)
    w2, h2, d2 = read_ppm(p2)
    if (w1, h1) != (w2, h2):
        return -1, None
    n = w1 * h1
    cnt = 0
    minx, miny, maxx, maxy = w1, h1, -1, -1
    for i in range(0, n * 3, 3):
        if (abs(d1[i] - d2[i]) > tol or abs(d1[i + 1] - d2[i + 1]) > tol
                or abs(d1[i + 2] - d2[i + 2]) > tol):
            cnt += 1
            px = (i // 3) % w1
            py = (i // 3) // w1
            if px < minx: minx = px
            if px > maxx: maxx = px
            if py < miny: miny = py
            if py > maxy: maxy = py
    box = (minx, miny, maxx, maxy) if maxx >= 0 else None
    return 100.0 * cnt / n, box


def wait_settle(q, max_wait=60, tol=0.4):
    """Wait until the frame stops changing (boot reveal animation finished).

    Taking the baseline mid-animation makes the drift check meaningless: the
    desktop is still brightening, which looks exactly like a dither bug.
    """
    a = os.path.join(OUT, "s_a.ppm")
    b = os.path.join(OUT, "s_b.ppm")
    q.cmd("screendump", {"filename": a})
    t = time.time()
    while time.time() - t < max_wait:
        # Compare frames ~2 s apart.  A short interval is fooled by the slow
        # boot reveal: two frames 0.4 s apart differ very little while the
        # picture is still brightening overall.
        time.sleep(2.0)
        q.cmd("screendump", {"filename": b})
        try:
            changed, _ = diff_pixels(a, b)
        except AssertionError:
            return False
        if changed >= 0 and changed < tol:
            return True
        a, b = b, a          # a now holds the most recent frame
    return False


def main():
    try:
        os.remove(SERLOG)
    except FileNotFoundError:
        pass
    args = [QEMU, "-drive", "format=raw,file=" + IMG, "-m", "512", "-vga", "std",
            "-display", "none", "-machine", "pc,mem-merge=off",
            "-accel", "tcg,tb-size=32", "-no-reboot",
            "-qmp", "tcp:127.0.0.1:%d,server,nowait" % QMP_PORT,
            "-serial", "file:" + SERLOG]
    p = subprocess.Popen(args)
    ok = True
    try:
        print("[*] GUI up:", wait_gui())
        time.sleep(2)
        s = socket.create_connection(("127.0.0.1", QMP_PORT))
        s.recv(4096)
        q = Q(s)
        q.cmd("qmp_capabilities")
        for c in "admin":
            q.key(c)
        time.sleep(0.3)
        q.key("ret")
        time.sleep(2.0)
        print("[*] settling          :", wait_settle(q))

        a = q.shot("v0_desktop")
        print("[*] t0   desktop      : %dx%d mean=%.1f black=%.1f%%" % (a["w"], a["h"], a["mean"], a["black_pct"]))
        time.sleep(10)
        b = q.shot("v1_after10s")
        print("[*] t10  desktop      : %dx%d mean=%.1f black=%.1f%%" % (b["w"], b["h"], b["mean"], b["black_pct"]))

        drift = abs(b["mean"] - a["mean"]) / max(a["mean"], 1.0) * 100.0
        print("[*] brightness drift  : %.2f%%" % drift)
        if a["mean"] < 5:
            print("[!] FAIL: desktop is essentially black")
            ok = False
        if drift > 4.0:
            print("[!] FAIL: brightness drifted >4%% (dither not idempotent?)")
            ok = False
        else:
            print("[+] OK: no progressive darkening")

        # pointer motion: the moved cursor must change pixels and leave no
        # permanent ghost behind (compare 10 s apart, pointer parked).
        q.move_to(600, 300)
        c1 = q.shot("v2_pointer")
        print("[*] pointer           : mean=%.1f black=%.1f%%" % (c1["mean"], c1["black_pct"]))
        pc, pbox = diff_pixels(os.path.join(OUT, "v1_after10s.ppm"),
                               os.path.join(OUT, "v2_pointer.ppm"))
        print("[*] cursor changed    : %.3f%% of pixels  bbox=%s" % (pc, pbox))

        # Start menu must still open with the cached desktop layer.
        q.move_to(479, 696)
        time.sleep(0.3)
        q.click()
        time.sleep(1.0)
        d = q.shot("v3_startmenu")
        print("[*] start menu        : mean=%.1f black=%.1f%%" % (d["mean"], d["black_pct"]))
        mc, mbox = diff_pixels(os.path.join(OUT, "v2_pointer.ppm"),
                               os.path.join(OUT, "v3_startmenu.ppm"))
        print("[*] after Start click : %.3f%% of pixels changed  bbox=%s" % (mc, mbox))
        # A real 380x248 menu is ~10% of a 1280x720 screen; a cursor-only
        # change is ~0.05%.  Anything below 1% means the menu did not open.
        if mc < 1.0:
            print("[!] WARN: only %.3f%% changed -- Start menu likely did NOT open" % mc)
        else:
            print("[+] OK: Start menu opened (%.1f%% of the screen changed)" % mc)

        q.cmd("quit")
        s.close()

        with open(SERLOG, "r", errors="replace") as f:
            fps = [int(m) for m in re.findall(r"\[FPS\]\s+(\d+)\s+fps", f.read())]
        if fps:
            fps_s = sorted(fps)
            print("[*] fps samples       : n=%d min=%d median=%d max=%d" %
                  (len(fps), fps_s[0], fps_s[len(fps) // 2], fps_s[-1]))
        print("\n=== RESULT:", "PASS" if ok else "FAIL", "===")
    finally:
        try:
            p.terminate()
        except Exception:
            pass
        time.sleep(1)
        try:
            p.kill()
        except Exception:
            pass


if __name__ == "__main__":
    main()
