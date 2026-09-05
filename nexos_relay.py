"""Relay: QEMU named-pipe serial <-> TCP, so the stdlib bridge can talk to the
kernel over a normal socket. Uses pywin32 (Python 3.10) for the pipe handle."""
import win32file, win32pipe, socket, threading, time, sys

PIPE = r"\\.\pipe\nexos_serial"
TCP_HOST = "127.0.0.1"
TCP_PORT = 4399
LOG = r"D:\MyOS\bootloader\relay_dbg.txt"

def log(s):
    with open(LOG, "a") as f:
        f.write(s + "\n")
    print(s, flush=True)

def main():
    open(LOG, "w").close()
    h = None
    for i in range(150):
        try:
            h = win32file.CreateFile(PIPE, win32file.GENERIC_READ | win32file.GENERIC_WRITE,
                                     0, None, win32file.OPEN_EXISTING, 0, None)
            log("[relay] pipe connected after %.1fs" % (i * 0.2))
            break
        except Exception:
            time.sleep(0.2)
    if not h:
        log("[relay] pipe never appeared; exiting"); sys.exit(1)
    win32pipe.SetNamedPipeHandleState(h, win32pipe.PIPE_READMODE_BYTE, None, None)

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((TCP_HOST, TCP_PORT))
    srv.listen(1)
    log("[relay] listening tcp %s:%d" % (TCP_HOST, TCP_PORT))

    while True:
        try:
            conn, _ = srv.accept()
        except Exception as e:
            log("[relay] accept err %s" % e); continue
        log("[relay] bridge connected")
        def pipe_to_tcp():
            try:
                while True:
                    ok, data = win32file.ReadFile(h, 4096, None)
                    if not data: break
                    conn.sendall(data)
            except Exception as e:
                log("[relay] pipe->tcp err %s" % e)
        def tcp_to_pipe():
            conn.settimeout(1.0)
            try:
                while True:
                    try:
                        data = conn.recv(4096)
                    except socket.timeout:
                        continue
                    except Exception:
                        break
                    if not data: break
                    try:
                        win32file.WriteFile(h, data)
                    except Exception as e:
                        log("[relay] tcp->pipe err %s" % e); break
            finally:
                try: conn.close()
                except Exception: pass
                log("[relay] bridge disconnected, waiting for reconnect")
        threading.Thread(target=pipe_to_tcp, daemon=True).start()
        tcp_to_pipe()

if __name__ == "__main__":
    main()
