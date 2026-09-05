import socket, subprocess, time, os

ROOT = r"D:\MyOS\bootloader"
QEMU = r"D:\qemu\qemu-system-x86_64.exe"
IMG = os.path.join(ROOT, "build", "os_v2.img")

def try_connect(label):
    try:
        s = socket.create_connection(("127.0.0.1", 4321), 0.5)
        print("[%s] CONNECTED" % label)
        return s
    except OSError:
        return None

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
    # race: try connecting repeatedly for 10s
    for i in range(50):
        s = try_connect("try%d" % i)
        if s:
            s.sendall(b"about\n")
            time.sleep(1.0)
            s.settimeout(2)
            try:
                d = s.recv(4096)
                print("GOT: " + repr(d[:200]))
            except socket.timeout:
                print("no data")
            s.close()
            break
        time.sleep(0.2)
    else:
        print("NEVER connected in 10s")
    q.terminate()

if __name__ == "__main__":
    main()
