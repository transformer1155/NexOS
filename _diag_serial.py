import socket, time, threading

PORT = 4401
s = socket.create_connection(("127.0.0.1", PORT), timeout=10)
print("[diag] connected to QEMU serial %d" % PORT, flush=True)
s.settimeout(1.0)
stop = threading.Event()
def reader():
    n = 0
    while not stop.is_set():
        try:
            d = s.recv(4096)
        except socket.timeout:
            continue
        except Exception:
            break
        if not d: break
        n += len(d)
        print("[k<-%d] %r" % (n, d.decode("utf-8","replace")[:400]), flush=True)
threading.Thread(target=reader, daemon=True).start()

time.sleep(35); s.sendall(b"whoami\n");    print("[diag] sent whoami", flush=True)
time.sleep(4);  s.sendall(b"login nexos nexos\n"); print("[diag] sent login", flush=True)
time.sleep(4);  s.sendall(b"ls\n");        print("[diag] sent ls", flush=True)
time.sleep(4);  s.sendall(b"echo SERIAL_OK\n"); print("[diag] sent echo", flush=True)
time.sleep(8)
stop.set(); s.close()
print("[diag] done", flush=True)
