"""Start-menu first-click regression test.

The bug: clicking Start the first time showed only the menu's background
panel; a second interaction was needed for the icons/text to appear.  Root
cause: the Start menu is owned by the C# overlay layer, which only flushes the
taskbar strip to the LFB, so the menu content painted above the strip was never
presented until a full repaint happened to occur.  Fix: gui_invalidate_managed()
now arms a frame-counted full-screen flush (g_over_force_n) so the open
animation is presented.

This test clicks the REAL taskbar Start button (bottom-left, C# coords
(20, height-24)), opens the menu once (frame A), closes and reopens (frame B),
and measures colour richness in the menu rectangle (native: x 8..388, y 34..282).
A full menu (panel + search box + pinned icon grid + text + power bar) yields a
high distinct-colour count; a background-only panel yields very few.  With the
fix, A must already be rich; without it A is nearly empty while B is rich.
"""
import subprocess, socket, json, time, os, re, glob

QEMU     = r"D:\qemu\qemu-system-x86_64.exe"
IMG      = r"D:\MyOS\bootloader\build\os_v2.img"
OUT      = r"D:\MyOS\bootloader\build"
QMP_PORT = 4466
SERLOG   = os.path.join(OUT, "sm_serial.log")

# native start-menu window rect (gui.cpp)
MX0, MY0, MW, MH = 8, 34, 380, 248


def read_ppm(path):
    with open(path, "rb") as f:
        assert f.readline().strip() == b"P6"
        wh = f.readline().split()
        w, h = int(wh[0]), int(wh[1])
        int(f.readline())
        data = f.read()
    return w, h, data


def region_color_buckets(ppm, x0, y0, x1, y1):
    """Return (distinct_bucket_count, total_sampled, top_colors) for a rect."""
    w, h, d = read_ppm(ppm)
    x0 = max(0, x0); y0 = max(0, y0)
    x1 = min(w - 1, x1); y1 = min(h - 1, y1)
    buckets = {}
    total = 0
    for y in range(y0, y1, 2):
        for x in range(x0, x1, 2):
            o = (y * w + x) * 3
            r, g, b = d[o], d[o + 1], d[o + 2]
            buckets[(r // 16, g // 16, b // 16)] = buckets.get((r // 16, g // 16, b // 16), 0) + 1
            total += 1
    top = sorted(buckets.items(), key=lambda kv: -kv[1])[:6]
    return len(buckets), total, [(c, n) for c, n in top]


def to_png(ppm, png):
    w, h, data = read_ppm(ppm)
    stride = w * 3
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        raw += data[y * stride:(y + 1) * stride]
    import zlib, struct
    comp = zlib.compress(bytes(raw), 9)
    def chunk(t, b):
        return struct.pack(">I", len(b)) + t + b + struct.pack(">I", zlib.crc32(t + b) & 0xffffffff)
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
            if not d: break
            buf += d
        return buf.decode(errors="replace")
    def rel(self, dx, dy):
        evs = []
        if dx: evs.append({"type": "rel", "data": {"axis": "x", "value": dx}})
        if dy: evs.append({"type": "rel", "data": {"axis": "y", "value": dy}})
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
            cx += sx; cy += sy; guard += 1
    def shot(self, name):
        ppm = os.path.join(OUT, name + ".ppm")
        self.cmd("screendump", {"filename": ppm})
        to_png(ppm, os.path.join(OUT, name + ".png"))
        return ppm


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
    for f in glob.glob(os.path.join(OUT, "sm_*.ppm")) + glob.glob(os.path.join(OUT, "sm_*.png")):
        try: os.remove(f)
        except OSError: pass
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
        time.sleep(3.0)

        # baseline + resolution
        base = q.shot("sm_base")
        w, h, _ = read_ppm(base)
        print("[*] resolution: %dx%d" % (w, h))
        sx, sy = 20, h - 24
        print("[*] Start button @ (%d, %d)" % (sx, sy))

        # ---- first open (the frame that used to show only the background) ----
        q.move_to(sx, sy); time.sleep(0.3); q.click(); time.sleep(1.4)
        a = q.shot("sm_openA")
        ca, ta, topa = region_color_buckets(a, MX0, MY0, MX0 + MW, MY0 + MH)
        print("[*] A (1st open)  menu-region distinct colours = %d / %d  top=%s" % (ca, ta, topa))

        # ---- close by clicking Start again ----
        q.move_to(sx, sy); time.sleep(0.3); q.click(); time.sleep(0.9)

        # ---- second open ----
        q.move_to(sx, sy); time.sleep(0.3); q.click(); time.sleep(1.4)
        b = q.shot("sm_openB")
        cb, tb, topb = region_color_buckets(b, MX0, MY0, MX0 + MW, MY0 + MH)
        print("[*] B (2nd open)  menu-region distinct colours = %d / %d  top=%s" % (cb, tb, topb))

        # baseline menu-region richness (should be near-zero: just wallpaper/desktop)
        cb0, tb0, top0 = region_color_buckets(base, MX0, MY0, MX0 + MW, MY0 + MH)
        print("[*] base desktop menu-region distinct colours = %d / %d" % (cb0, tb0))

        # decision
        RICH = 18   # a real Win11 start menu has far more than this
        if ca < RICH:
            print("[!] FAIL: 1st-open menu region is poor (%d < %d) -- icons/text not presented" % (ca, RICH))
            ok = False
        if abs(ca - cb) > 6:
            print("[!] WARN: 1st-open (%d) and 2nd-open (%d) disagree -- inconsistent" % (ca, cb))
        if ca >= RICH and abs(ca - cb) <= 6:
            print("[+] OK: first click already shows a full Start menu (%d ~= %d)" % (ca, cb))
        print("\n=== RESULT:", "PASS" if ok else "FAIL", "===")
        q.cmd("quit")
        s.close()
    finally:
        try: p.terminate()
        except Exception: pass
        time.sleep(1)
        try: p.kill()
        except Exception: pass


if __name__ == "__main__":
    main()
