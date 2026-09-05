import win32file, win32pipe, time

PIPE = r"\\.\pipe\nexos_serial"

def main():
    # QEMU (server) created the pipe. Open it as client.
    try:
        h = win32file.CreateFile(
            PIPE,
            win32file.GENERIC_READ | win32file.GENERIC_WRITE,
            0, None,
            win32file.OPEN_EXISTING,
            0, None
        )
        print("[pipe] connected")
    except Exception as e:
        print("[pipe] open failed: %s" % e)
        return

    win32pipe.SetNamedPipeHandleState(h, win32pipe.PIPE_READMODE_BYTE, None, None)

    time.sleep(3)
    for c in ["about", "ls", "users", "help"]:
        print("=== send %s ===" % c)
        win32file.WriteFile(h, (c + "\n").encode())
        time.sleep(1.2)
        buf = b""
        try:
            while True:
                ok, data = win32file.ReadFile(h, 4096, None)
                if not data:
                    break
                buf += data
        except Exception:
            pass
        print("[resp] " + repr(buf[:300]))
        print()
    win32file.CloseHandle(h)

if __name__ == "__main__":
    main()
