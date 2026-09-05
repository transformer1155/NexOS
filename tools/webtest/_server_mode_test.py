import socket, subprocess, time, os

# Act as the serial-side TCP SERVER (port 4321), let QEMU connect as client.
ROOT = r"D:\MyOS\bootloader"
QEMU = r"D:\qemu\qemu-system-x86_64.exe"
IMG = os.path.join(ROOT, "build", "os_v2.img")

def main():
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", 4321))
    srv.listen(1)
    print("[server] listening on 4321")

    q = subprocess.Popen([
        QEMU, "-drive", "format=raw,file=" + IMG,
        "-m", "128", "-vga", "std", "-display", "none",
        "-machine", "pc,mem-merge=off",
        "-chardev", "socket,id=ser0,connect=on,host=127.0.0.1,port=4321",
        "-serial", "chardev:ser0",
        "-accel", "tcg,tb-size=32", "-no-reboot",
    ])
    print("[server] QEMU pid=%s" % q.pid)

    # accept QEMU's client connection
    srv.settimeout(15)
    try:
        conn, addr = srv.accept()
    except socket.timeout:
        print("[server] QEMU never connected!")
        q.terminate()
        return
    print("[server] QEMU connected from %s" % str(addr))
    conn.settimeout(2)

    time.sleep(3)
    for c in ["about", "ls", "users", "help"]:
        print("=== send %s ===" % c)
        conn.sendall((c + "\n").encode())
        time.sleep(1.5)
        buf = b""
        try:
            while True:
                d = conn.recv(4096)
                if not d:
                    print("[server] connection closed by QEMU!")
                    break
                buf += d
        except socket.timeout:
            pass
        print("[resp] " + repr(buf[:300]))
        print()

    conn.close()
    srv.close()
    q.terminate()

if __name__ == "__main__":
    main()
