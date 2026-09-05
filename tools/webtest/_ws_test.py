import base64, hashlib, socket, struct, time

GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


def main():
    s = socket.create_connection(("127.0.0.1", 8765), 5)
    key = base64.b64encode(b"t").decode()
    s.sendall(
        ("GET / HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
         "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n" % key).encode())
    r = b""
    while b"\r\n\r\n" not in r:
        r += s.recv(1)

    def fr(timeout=5):
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

    for c in ["about", "ls", "users", "help"]:
        print("=== send via bridge: %s ===" % c)
        fs(c)
        drain("resp", 2.0)
    s.close()


if __name__ == "__main__":
    main()
