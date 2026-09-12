"""Regression test for the Start-menu "first click shows only the panel
background" bug.

The overlay (Shell::PaintOverlay) draws the taskbar AND the Start menu, but the
damage it reported was only the taskbar strip.  The menu was therefore painted
into the backbuffer and never presented, so the user kept seeing the panel that
a previous full repaint had left behind -- until the next click forced another
full flush, which made the second click look correct.

Externally that is measured as CONTENT, not just "something changed": a bare
panel is a flat rounded rectangle (a couple of distinct colours), while a real
menu has icons and labels (hundreds).  The menu rectangle is located by
diffing against the closed desktop, so no hard-coded coordinates are needed.
"""
import subprocess, socket, json, time, os, zlib, struct

QEMU     = r"D:\qemu\qemu-system-x86_64.exe"
IMG      = r"D:\MyOS\bootloader\build\os_v2.img"
OUT      = r"D:\MyOS\bootloader\build"
QMP_PORT = 4467
SERLOG   = os.path.join(OUT, "menu_serial.log")


def read_ppm(path):
    with open(path, "rb") as f:
        assert f.readline().strip() == b"P6"
        wh = f.readline().split()
        w, h = int(wh[0]), int(wh[1])
        int(f.readline())
        return w, h, f.read()


def changed_bbox(p1, p2, tol=10):
    """Pixels that differ between two frames + their bounding box."""
    w1, h1, d1 = read_ppm(p1)
    w2, h2, d2 = read_ppm(p2)
    if (w1, h1) != (w2, h2):
        return 0.0, None
    cnt = 0
    minx, miny, maxx, maxy = w1, h1, -1, -1
    for i in range(0, w1 * h1 * 3, 3):
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
    return 100.0 * cnt / (w1 * h1), box


def detail(path, box):
    """Colour richness inside box: distinct quantised colours + the fraction
    of pixels that are NOT the modal colour."""
    if not box:
        return 0, 0.0
    w, h, d = read_ppm(path)
    x0, y0, x1, y1 = box
    x0 = max(0, x0); y0 = max(0, y0)
    x1 = min(x1, w - 1); y1 = min(y1, h - 1)
    hist = {}
    total = 0
    for y in range(y0, y1 + 1):
        base = y * w * 3
        for x in range(x0, x1 + 1):
            i = base + x * 3
            key = (d[i] >> 3, d[i + 1] >> 3, d[i + 2] >> 3)
            hist[key] = hist.get(key, 0) + 1
            total += 1
    if not total:
        return 0, 0.0
    modal = max(hist.values())
    return len(hist), 1.0 - modal / float(total)


def screen_colors(path):
    w, h, d = read_ppm(path)
    hist = {}
    for i in range(0, w * h * 3, 3):
        hist[(d[i] >> 3, d[i + 1] >> 3, d[i + 2] >> 3)] = 1
    return len(hist)


def samples(path):
    """Mean brightness + raw pixel values at a few probe points."""
    w, h, d = read_ppm(path)
    tot = 0
    raw = {}
    for i in range(0, w * h * 3, 3):
        tot += d[i] + d[i + 1] + d[i + 2]
        raw[(d[i], d[i + 1], d[i + 2])] = 1
    pts = [(640, 100), (640, 300), (640, 500), (100, 650), (1200, 700)]
    out = []
    for (px, py) in pts:
        i = (py * w + px) * 3
        out.append("(%d,%d)=%d,%d,%d" % (px, py, d[i], d[i + 1], d[i + 2]))
    return tot / (w * h * 3.0), len(raw), "  ".join(out)


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
    try:
        os.remove(ppm)      # 2.7 MB each; keeping them fills the disk
    except OSError:
        pass


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
        try:
            w, h, _ = read_ppm(ppm)
        except Exception:
            return None
        tmp = os.path.join(OUT, name + ".copy.ppm")
        data = open(ppm, "rb").read()
        open(tmp, "wb").write(data)
        to_png(ppm, os.path.join(OUT, name + ".png"))
        return tmp


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


def main():
    try:
        os.remove(SERLOG)
    except FileNotFoundError:
        pass
    for f in os.listdir(OUT):
        if f.endswith(".copy.ppm"):
            try:
                os.remove(os.path.join(OUT, f))
            except OSError:
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
        time.sleep(6.0)                 # let the boot reveal finish

        base = q.shot("m0_closed")
        mean, rawc, pts = samples(base)
        print("[*] closed  : screen colors=%d  rawcolors=%d  mean=%.1f" %
              (screen_colors(base), rawc, mean))
        print("[*] probes  : %s" % pts)

        # ONE click on Start -- the case that used to show only the panel.
        q.move_to(479, 696)
        time.sleep(0.3)
        q.click()
        time.sleep(1.5)                 # open animation + settle
        m1 = q.shot("m1_first_click")

        pct, box = changed_bbox(base, m1)
        print("[*] after 1 click: %.2f%% of the screen changed  bbox=%s" % (pct, box))
        c1, f1 = detail(m1, box)
        print("[*] menu content : colors=%-5d non-modal=%.3f" % (c1, f1))

        if pct < 3.0:
            print("[!] FAIL: menu did not open (only %.2f%% changed)" % pct)
            ok = False
        elif c1 < 20 or f1 < 0.05:
            print("[!] FAIL: menu is a flat panel after ONE click "
                  "(colors=%d non-modal=%.3f) -> background-only bug" % (c1, f1))
            ok = False
        else:
            print("[+] OK: menu has real content after ONE click "
                  "(colors=%d non-modal=%.3f)" % (c1, f1))

        q.cmd("quit")
        s.close()
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
