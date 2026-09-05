import socket, time
# Test whether QEMU delivers GUEST->host serial TX to the tcp client at all.
s = socket.create_connection(("127.0.0.1", 4402), timeout=10)
print("[tx] connected 4402", flush=True)
s.settimeout(2.0)
total = 0
t0 = time.time()
try:
    while time.time() - t0 < 40:
        try:
            d = s.recv(4096)
        except socket.timeout:
            continue
        if not d: break
        total += len(d)
        print("[tx<-%d] %r" % (total, d.decode("utf-8","replace")[:300]), flush=True)
except Exception as e:
    print("[tx] err", e, flush=True)
print("[tx] total bytes from guest:", total, flush=True)
s.close()
