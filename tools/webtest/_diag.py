import base64, hashlib, socket, struct, subprocess, time, os, signal

GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
ROOT = r"D:\MyOS\bootloader"
QEMU = r"D:\qemu\qemu-system-x86_64.exe"
IMG = os.path.join(ROOT, "build", "os_v2.img")
BRIDGE = os.path.join(ROOT, "tools", "nexos_bridge.py")


def start_qemu():
    args = [
        QEMU, "-drive", "format=raw,file=" + IMG,
        "-m", "128", "-vga", "std", "-display", "none",
        "-machine", "pc,mem-merge=off",
        "-chardev", "socket,id=ser0,server=on,wait=off,host=127.0.0.1,port=4321",
        "-serial", "chardev:ser0",
        "-accel", "tcg,tb-size=32", "-no-reboot",
    ]
    return subprocess.Popen(args)


def start_bridge():
    return subprocess.Popen(["python", BRIDGE], cwd=ROOT)


def ws_probe():
    s = socket.create_connection(("127.0.0.1", 8765), 5)
    key = base64.b64encode(b"t").decode()
    s.sendall(
        ("GET / HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
         "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n" % key).encode())
    r = b""
    while b"\r\n\r\n" not in r:
        r += s.recv(1)

    def fr(timeout=4):
        s.settimeout(timeout)
        try:
            h = s.recv(2)
            if len(h) < 2:
                return None
            o = h[0] & 0x0F
            l = h[1] & 0x7F
            if l == 126:
                l = struct.unpack(">H", s.recv(2))[0]
            elif l == 127:
                l = struct.unpack(">Q", s.recv(8))[0]
            d = b""
            while len(d) < l:
                d += s.recv(l - len(d))
            return d.decode("utf-8", "replace") if o != 8 else None
        except Exception:
            return None

    def fs(t):
        p = t.encode()
        L = len(p)
        hdr = struct.pack(">BB", 0x81, L) if L < 126 else struct.pack(">BBH", 0x81, 126, L)
        s.sendall(hdr + p)

    def drain(label, t=2.0):
        time.sleep(t)
        out = []
        while True:
            x = fr()
            if x is None:
                break
            out.append(x)
        print("[%s] %s" % (label, repr("".join(out)[:500])))
        print()

    print("=== initial kernel output (wait 4s) ===")
    drain("init", 4.0)
    for c in ["about", "ls", "users", "help"]:
        print("=== send: %s ===" % c)
        fs(c)
        drain("out", 2.0)
    s.close()


def main():
    q = start_qemu()
    b = start_bridge()
    print("QEMU pid=%s bridge pid=%s" % (q.pid, b.pid))
    time.sleep(12)  # let kernel fully boot
    try:
        ws_probe()
    finally:
        b.terminate()
        q.terminate()


if __name__ == "__main__":
    main()
