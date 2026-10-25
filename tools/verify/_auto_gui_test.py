import os, socket, subprocess, sys, time

BASE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(BASE)

def main():
    os.chdir(ROOT)
    for f in ["/tmp/qmon.sock", "build/serial_final.log", "build/scr_final.ppm"]:
        try:
            os.remove(f)
        except FileNotFoundError:
            pass

    cmd = [
        "qemu-system-x86_64",
        "-drive", "format=raw,file=build/os.img",
        "-m", "4096",
        "-vga", "std",
        "-display", "none",
        "-serial", "file:build/serial_final.log",
        "-monitor", "unix:/tmp/qmon.sock,server,nowait",
        "-no-reboot",
        "-net", "none",
    ]
    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        # Wait for QEMU to create the monitor socket and boot to login.
        for _ in range(40):
            if os.path.exists("/tmp/qmon.sock"):
                break
            time.sleep(0.5)
        if not os.path.exists("/tmp/qmon.sock"):
            print("Monitor socket did not appear")
            return 1

        time.sleep(12)  # let boot reach login prompt

        s = socket.socket(socket.AF_UNIX)
        s.connect("/tmp/qmon.sock")
        time.sleep(0.5)
        s.settimeout(0.5)
        try:
            s.recv(4096)
        except socket.timeout:
            pass

        def sendkey(c):
            s.sendall(("sendkey " + c + "\n").encode())
            time.sleep(0.35)

        for c in "root": sendkey(c)
        sendkey("ret")
        time.sleep(1.5)
        for c in "admin": sendkey(c)
        sendkey("ret")
        print("login injected")

        # Wait for auto-GUI to bring up the desktop.
        time.sleep(10)
        s.sendall(b"screendump build/scr_final.ppm\n")
        time.sleep(1)
        s.close()
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()

    # Verify markers.
    log = open("build/serial_final.log", "rb").read().decode("latin-1", "ignore")
    ok = "[K32] Entering Win11 GUI mode" in log and "[GUI] Entered GUI mode" in log
    print("GUI markers present:", ok)
    if not ok:
        print("--- serial tail ---")
        print("\n".join(log.splitlines()[-25:]))
        return 1
    return 0

if __name__ == "__main__":
    sys.exit(main())
