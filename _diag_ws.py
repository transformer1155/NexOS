import socket, struct, base64, os, time, threading

GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

def ws_connect(host, port):
    s = socket.create_connection((host, port), timeout=5)
    key = base64.b64encode(os.urandom(16)).decode()
    req = ("GET / HTTP/1.1\r\nHost: %s:%d\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
           "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n" % (host, port, key))
    s.sendall(req.encode())
    buf = b""
    while b"\r\n\r\n" not in buf:
        buf += s.recv(1)
    return s

def ws_send(s, payload):
    data = payload.encode("utf-8"); n = len(data)
    if n < 126: s.sendall(struct.pack(">BB", 0x81, n) + data)
    elif n < 65536: s.sendall(struct.pack(">BBH", 0x81, 126, n) + data)
    else: s.sendall(struct.pack(">BQB", 0x81, 127, n) + data)

def reader(s, stop):
    s.settimeout(1.0)
    while not stop.is_set():
        try:
            hdr = s.recv(2)
        except socket.timeout:
            continue
        except Exception:
            break
        if len(hdr) < 2: break
        plen = hdr[1] & 0x7F
        if plen == 126: plen = struct.unpack(">H", s.recv(2))[0]
        elif plen == 127: plen = struct.unpack(">Q", s.recv(8))[0]
        data = b""
        while len(data) < plen:
            data += s.recv(plen - len(data))
        print("[ws<-] %r" % data.decode("utf-8", "replace"), flush=True)

s = ws_connect("127.0.0.1", 8765)
print("[ws] connected", flush=True)
stop = threading.Event()
t = threading.Thread(target=reader, args=(s, stop), daemon=True)
t.start()

# Wait for kernel to reach GUI mode, then drive it.
time.sleep(20); ws_send(s, "login nexos nexos\n"); print("[ws] sent login", flush=True)
time.sleep(8);  ws_send(s, "whoami\n");                 print("[ws] sent whoami", flush=True)
time.sleep(8);  ws_send(s, "ls\n");                     print("[ws] sent ls", flush=True)
time.sleep(10)
stop.set()
s.close()
print("[ws] done", flush=True)
