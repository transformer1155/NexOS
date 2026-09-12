import subprocess, socket, json, time, os, sys, zlib, struct

QEMU  = r"D:\qemu\qemu-system-x86_64.exe"
IMG   = r"D:\MyOS\bootloader\build\os_v2.img"
OUT   = r"D:\MyOS\bootloader\build"
QMP_PORT = 4455
SERLOG   = os.path.join(OUT, "qemu_serial.log")
SHOT1    = os.path.join(OUT, "shot1.ppm")
SHOT2    = os.path.join(OUT, "shot2.ppm")
PNG1     = os.path.join(OUT, "shot1.png")
PNG2     = os.path.join(OUT, "shot2.png")

def ppm_to_png(ppm, png):
    with open(ppm, "rb") as f:
        assert f.readline().strip() == b"P6", "not P6"
        wh = f.readline().split()
        w = int(wh[0]); h = int(wh[1])
        int(f.readline())  # maxval
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
    png_out = bytearray(b"\x89PNG\r\n\x1a\n")
    png_out += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png_out += chunk(b"IDAT", comp)
    png_out += chunk(b"IEND", b"")
    with open(png, "wb") as f:
        f.write(png_out)

def qmp(sock, cmd, args=None):
    obj = {"execute": cmd}
    if args is not None:
        obj["arguments"] = args
    sock.sendall((json.dumps(obj) + "\r\n").encode())
    buf = b""
    while b'"return"' not in buf and b'"error"' not in buf:
        d = sock.recv(4096)
        if not d:
            break
        buf += d
    return buf.decode(errors="replace")

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
    try:
        os.remove(SERLOG)
    except FileNotFoundError:
        pass
    args = [
        QEMU,
        "-drive", "format=raw,file=" + IMG,
        "-m", "512", "-vga", "std", "-display", "none",
        "-machine", "pc,mem-merge=off",
        "-accel", "tcg,tb-size=32", "-no-reboot",
        "-qmp", "tcp:127.0.0.1:%d,server,nowait" % QMP_PORT,
        "-serial", "file:" + SERLOG,
    ]
    p = subprocess.Popen(args)
    try:
        print("[*] waiting for GUI...")
        ok = wait_gui()
        print("[*] GUI up:", ok)
        time.sleep(2)
        s = socket.create_connection(("127.0.0.1", QMP_PORT))
        s.recv(4096)  # greeting
        qmp(s, "qmp_capabilities")
        # shot 1: initial (cursor at center)
        qmp(s, "screendump", {"filename": SHOT1})
        time.sleep(1)
        # move mouse right + down
        qmp(s, "input-send-event", {"events": [
            {"type": "rel", "data": {"axis": "x", "value": 320}},
            {"type": "rel", "data": {"axis": "y", "value": 200}},
        ]})
        time.sleep(1.5)
        # shot 2: after move
        qmp(s, "screendump", {"filename": SHOT2})
        time.sleep(1)
        qmp(s, "quit")
        s.close()
        ppm_to_png(SHOT1, PNG1)
        ppm_to_png(SHOT2, PNG2)
        print("[*] wrote", PNG1, PNG2)
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
