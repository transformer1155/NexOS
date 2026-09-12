import subprocess, socket, json, time, os, sys, zlib, struct

QEMU  = r"D:\qemu\qemu-system-x86_64.exe"
IMG   = r"D:\MyOS\bootloader\build\os_v2.img"
OUT   = r"D:\MyOS\bootloader\build"
QMP_PORT = 4457
SERLOG   = os.path.join(OUT, "click_serial.log")
SHOT0    = os.path.join(OUT, "click0.ppm")
SHOT1    = os.path.join(OUT, "click1.ppm")
PNG0     = os.path.join(OUT, "click0.png")
PNG1     = os.path.join(OUT, "click1.png")

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

def qmp(sock, cmd, args=None):
    obj = {"execute": cmd}
    if args is not None:
        obj["arguments"] = args
    sock.sendall((json.dumps(obj) + "\r\n").encode())
    buf = b""
    while b'"return"' not in buf and b'"error"' not in buf:
        d = sock.recv(4096)
        if not d: break
        buf += d
    return buf.decode(errors="replace")

def rel(dx, dy):
    evs = []
    if dx: evs.append({"type":"rel","data":{"axis":"x","value":dx}})
    if dy: evs.append({"type":"rel","data":{"axis":"y","value":dy}})
    return evs

def click(down):
    return [{"type":"btn","data":{"button":"left","down":down}}]

def key_press(qcode):
    return [
        {"type":"key","data":{"key":{"type":"qcode","data":qcode},"down":True}},
        {"type":"key","data":{"key":{"type":"qcode","data":qcode},"down":False}},
    ]

def wait_gui(timeout=30):
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
    try: os.remove(SERLOG)
    except FileNotFoundError: pass
    args = [QEMU, "-drive", "format=raw,file=" + IMG, "-m", "512", "-vga", "std",
            "-display", "none", "-machine", "pc,mem-merge=off", "-accel", "tcg,tb-size=32",
            "-no-reboot", "-qmp", "tcp:127.0.0.1:%d,server,nowait" % QMP_PORT,
            "-serial", "file:" + SERLOG]
    p = subprocess.Popen(args)
    try:
        print("[*] GUI up:", wait_gui())
        time.sleep(2)
        s = socket.create_connection(("127.0.0.1", QMP_PORT))
        s.recv(4096)
        qmp(s, "qmp_capabilities")
        qmp(s, "screendump", {"filename": SHOT0})
        time.sleep(1)
        # Move from center (640,360) down to password field ~ y=420
        qmp(s, "input-send-event", {"events": rel(0, 60)})
        time.sleep(0.3)
        # left click on password field
        qmp(s, "input-send-event", {"events": click(True) + click(False)})
        time.sleep(0.3)
        # type "nexos"
        evs = []
        for c in "nexos": evs += key_press(c)
        qmp(s, "input-send-event", {"events": evs})
        time.sleep(1)
        qmp(s, "screendump", {"filename": SHOT1})
        time.sleep(1)
        qmp(s, "quit")
        s.close()
        ppm_to_png(SHOT0, PNG0)
        ppm_to_png(SHOT1, PNG1)
        print("[*] wrote", PNG0, PNG1)
    finally:
        try: p.terminate()
        except Exception: pass
        time.sleep(1)
        try: p.kill()
        except Exception: pass

if __name__ == "__main__":
    main()
