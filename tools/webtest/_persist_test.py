import socket, subprocess, time, os

ROOT = r"D:\MyOS\bootloader"
QEMU = r"D:\qemu\qemu-system-x86_64.exe"
IMG = os.path.join(ROOT, "build", "os_v2.img")

def main():
    q = subprocess.Popen([
        QEMU, "-drive", "format=raw,file=" + IMG,
        "-m", "128", "-vga", "std", "-display", "none",
        "-machine", "pc,mem-merge=off",
        "-chardev", "socket,id=ser0,server=on,wait=off,host=127.0.0.1,port=4321",
        "-serial", "chardev:ser0",
        "-accel", "tcg,tb-size=32", "-no-reboot",
    ])
    print("QEMU pid=%s" % q.pid)

    s = None
    for i in range(80):
        try:
            s = socket.create_connection(("127.0.0.1", 4321), 0.5)
            print("[connect] at try %d (%.1fs)" % (i, i * 0.2))
            break
        except OSError:
            time.sleep(0.2)
    if not s:
        print("never connected"); q.terminate(); return

    s.settimeout(1)
    # wait for kernel to be ready (poll with empty cmd? just sleep)
    time.sleep(4)
    s.sendall(b"about\n")
    # now read continuously, log each recv chunk size
    last = time.time()
    total = 0
    while time.time() - last < 6:
        try:
            d = s.recv(4096)
            if not d:
                print("[recv] EMPTY at %.1fs (connection closed by QEMU)" % (time.time()))
                break
            total += len(d)
            print("[recv] %d bytes at %.1fs: %s" % (len(d), time.time(), repr(d[:60])))
            last = time.time()
        except socket.timeout:
            print("[recv] timeout (no data %.1fs, total=%d)" % (time.time(), total))
            last = time.time()
    s.close()
    q.terminate()

if __name__ == "__main__":
    main()
