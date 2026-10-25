#!/usr/bin/env python3
import os, sys, socket, time, subprocess
ROOT = "/mnt/d/MyOS/bootloader"
os.chdir(ROOT)
ISO = sys.argv[1] if len(sys.argv) > 1 else "build/os.iso"
SER = "/tmp/dbg_ser.log"
PORT = 4499

def mon_cmd(mon, cmd):
    mon.sendall((cmd + "\n").encode())
    time.sleep(0.6)

def wait_sock(port, timeout=30.0):
    end = time.time() + timeout
    while time.time() < end:
        try:
            return socket.create_connection(("127.0.0.1", port), timeout=0.5)
        except OSError:
            time.sleep(0.2)
    raise RuntimeError("monitor not ready")

def main():
    if os.path.exists(SER):
        os.remove(SER)
    qemu = subprocess.Popen([
        "qemu-system-x86_64", "-cdrom", ISO, "-boot", "d", "-m", "128M",
        "-vga", "std", "-display", "none", "-no-reboot",
        "-monitor", f"tcp:127.0.0.1:{PORT},server,nowait",
        "-chardev", f"file,id=ser,path={SER}", "-serial", "chardev:ser",
    ])
    try:
        mon = wait_sock(PORT)
        mon.settimeout(3.0)
        try: mon.recv(65536)
        except Exception: pass
        # early screendump to see [CDBoot] text
        time.sleep(4.0)
        mon_cmd(mon, "screendump /tmp/dbg_early.ppm")
        time.sleep(16.0)  # total ~20s
        data = open(SER, "rb").read().decode("latin-1", "ignore")
        mon_cmd(mon, "screendump /tmp/dbg_mid.ppm")
        print("=== serial len", len(data), "===")
        print(data[:4000])
        print("=== tail serial ===")
        print(data[-1500:])
        mon_cmd(mon, "quit")
    finally:
        try: qemu.wait(timeout=5.0)
        except Exception:
            qemu.terminate(); qemu.wait(timeout=3.0)

if __name__ == "__main__":
    main()
