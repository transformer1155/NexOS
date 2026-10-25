import subprocess, socket, json, time, os, sys, zlib, struct

QEMU  = r"D:\qemu\qemu-system-x86_64.exe"
IMG   = r"D:\MyOS\bootloader\build\os_v2.img"
OUT   = r"D:\MyOS\bootloader\build"
QMP_PORT = 4463
SERLOG   = os.path.join(OUT, "sm_serial.log")
SHOTS = {0:"sm0.png", 1:"sm1.png", 2:"sm2.png", 3:"sm3.png"}

def ppm_to_png(ppm, png):
    with open(ppm, "rb") as f:
        assert f.readline().strip() == b"P6"
        wh = f.readline().split()
        w = int(wh[0]); h = int(wh[1])
        int(f.readline())
        data = f.read()
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
    def __init__(self, sock): self.s = sock
    def cmd(self, c, a=None):
        obj = {"execute": c}
        if a is not None: obj["arguments"] = a
        self.s.sendall((json.dumps(obj) + "\r\n").encode())
        buf = b""
        while b'"return"' not in buf and b'"error"' not in buf:
            d = self.s.recv(4096)
            if not d: break
            buf += d
        return buf.decode(errors="replace")
    def rel(self, dx, dy):
        evs = []
        if dx: evs.append({"type":"rel","data":{"axis":"x","value":dx}})
        if dy: evs.append({"type":"rel","data":{"axis":"y","value":dy}})
        if evs:
            # send ONE event per call, slow enough that QEMU's 1-byte PS/2
            # mouse output buffer is drained by the kernel between events.
            for e in evs:
                self.cmd("input-send-event", {"events":[e]})
                time.sleep(0.09)
    def click(self):
        self.cmd("input-send-event", {"events":[{"type":"btn","data":{"button":"left","down":True}}]})
        time.sleep(0.05)
        self.cmd("input-send-event", {"events":[{"type":"btn","data":{"button":"left","down":False}}]})
        time.sleep(0.05)
    def key(self, qc):
        self.cmd("input-send-event", {"events":[
            {"type":"key","data":{"key":{"type":"qcode","data":qc},"down":True}},
            {"type":"key","data":{"key":{"type":"qcode","data":qc},"down":False}}]})
        time.sleep(0.03)
    def move_to(self, x, y):
        # Step from the current (unknown) position toward (x,y) in small
        # increments.  QEMU's PS/2 mouse buffer is 1 byte, so every event must
        # be drained by the guest before the next; the 0.09s sleep in rel()
        # handles that.  We never issue a jump larger than ~100px, so no
        # packet is ever split/desynced.
        # First walk back toward (0,0) in safe <=100 steps so we start from a
        # known corner.
        for _ in range(40):
            self.rel(-100, -100)
        time.sleep(0.1)
        cx, cy = 0, 0
        guard = 0
        while (cx != x or cy != y) and guard < 200:
            sx = max(-100, min(100, x - cx))
            sy = max(-100, min(100, y - cy))
            self.rel(sx, sy)
            cx += sx; cy += sy; guard += 1
    def shot(self, n):
        ppm = os.path.join(OUT, "sm%d.ppm" % n)
        self.cmd("screendump", {"filename": ppm})
        ppm_to_png(ppm, os.path.join(OUT, SHOTS[n]))

def wait_gui(timeout=30):
    t = time.time()
    while time.time() - t < timeout:
        try:
            with open(SERLOG, "r", errors="replace") as f:
                if "Entered GUI mode" in f.read(): return True
        except FileNotFoundError:
            pass
        time.sleep(0.5)
    return False

def main():
    try: os.remove(SERLOG)
    except FileNotFoundError: pass
    args = [QEMU, "-drive", "format=raw,file="+IMG, "-m", "512", "-vga", "std",
            "-display", "none", "-machine", "pc,mem-merge=off", "-accel", "tcg,tb-size=32",
            "-no-reboot", "-qmp", "tcp:127.0.0.1:%d,server,nowait" % QMP_PORT,
            "-serial", "file:"+SERLOG]
    p = subprocess.Popen(args)
    try:
        print("[*] GUI up:", wait_gui())
        time.sleep(2)
        s = socket.create_connection(("127.0.0.1", QMP_PORT))
        s.recv(4096)
        q = Q(s)
        q.cmd("qmp_capabilities")
        q.shot(0)                      # login baseline
        for c in "admin": q.key(c)     # password
        time.sleep(0.3)
        q.key("ret")                   # submit
        time.sleep(1.5)
        q.shot(1)                      # desktop
        # Start button: taskbar centered, GroupX=459, btn box (459,676,40,40)
        q.move_to(479, 696)
        time.sleep(0.2)
        q.click()
        time.sleep(0.8)
        q.shot(2)                      # start menu
        # first start tile: menu rect x[380,900] y[232,662]; tile0 ~ (408,326)
        q.move_to(466, 368)
        time.sleep(0.2)
        q.click()
        time.sleep(0.8)
        q.shot(3)                      # after tile click
        q.cmd("quit")
        s.close()
        print("[*] wrote", [SHOTS[i] for i in range(4)])
    finally:
        try: p.terminate()
        except Exception: pass
        time.sleep(1)
        try: p.kill()
        except Exception: pass

if __name__ == "__main__":
    main()
