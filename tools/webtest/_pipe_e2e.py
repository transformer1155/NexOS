import win32file, win32pipe, subprocess, time, os

ROOT = r"D:\MyOS\bootloader"
QEMU = r"D:\qemu\qemu-system-x86_64.exe"
IMG = os.path.join(ROOT, "build", "os_v2.img")
PIPE = r"\\.\pipe\nexos_serial"

def main():
    q = subprocess.Popen([
        QEMU, "-drive", "format=raw,file=" + IMG,
        "-m", "128", "-vga", "std", "-display", "none",
        "-machine", "pc,mem-merge=off",
        "-chardev", "pipe,id=ser0,path=nexos_serial", "-serial", "chardev:ser0",
        "-accel", "tcg,tb-size=32", "-no-reboot",
    ])
    print("QEMU pid=%s" % q.pid)

    # wait for pipe to appear
    h = None
    for i in range(50):
        try:
            h = win32file.CreateFile(PIPE, win32file.GENERIC_READ | win32file.GENERIC_WRITE,
                                     0, None, win32file.OPEN_EXISTING, 0, None)
            print("[pipe] connected at %.1fs" % (i * 0.2))
            break
        except Exception:
            time.sleep(0.2)
    if not h:
        print("pipe never appeared"); q.terminate(); return

    win32pipe.SetNamedPipeHandleState(h, win32pipe.PIPE_READMODE_BYTE, None, None)
    time.sleep(3)
    for c in ["about", "ls", "users", "help"]:
        print("=== send %s ===" % c)
        win32file.WriteFile(h, (c + "\n").encode())
        time.sleep(1.2)
        buf = b""
        end = time.time() + 2.0
        while time.time() < end:
            try:
                ok, data = win32file.ReadFile(h, 4096, None)
                if not data:
                    break
                buf += data
            except Exception:
                break
        print("[resp] " + repr(buf[:300]))
        print()
    win32file.CloseHandle(h)
    q.terminate()

if __name__ == "__main__":
    main()
