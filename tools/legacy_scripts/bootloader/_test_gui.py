#!/usr/bin/env python3
"""One-shot QEMU test for MiniOS GUI launch."""
import subprocess, socket, time, os, sys

# Cleanup
os.system("rm -f /tmp/mon.sock /tmp/shot.ppm 2>/dev/null")

# Launch QEMU
p = subprocess.Popen(
    ["qemu-system-x86_64", "-cdrom", "build/os.iso", "-boot", "d",
     "-m", "2G", "-display", "none",
     "-monitor", "unix:/tmp/mon.sock,server,nowait",
     "-serial", "file:/tmp/ser.txt", "-no-reboot"],
    stdout=open("/dev/null"), stderr=subprocess.STDOUT)

# Wait for QEMU to boot kernel and reach shell
time.sleep(25)

# Connect to monitor
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
for i in range(20):
    try:
        s.connect("/tmp/mon.sock")
        break
    except Exception:
        time.sleep(1)
else:
    print("Cannot connect to monitor")
    p.kill()
    sys.exit(1)

# sendkey "run test.bat"
for k in "run test.bat":
    name = {" ": "spc", ".": "dot"}.get(k, k)
    s.sendall(("sendkey %s\n" % name).encode())
    time.sleep(0.05)
s.sendall(b"sendkey ret\n")

# Wait for GUI to open + window to render
time.sleep(15)

# Screendump
s.sendall(b"screendump /tmp/shot.ppm\n")
time.sleep(2)
s.sendall(b"quit\n")
s.close()
p.wait(timeout=15)

if os.path.exists("/tmp/shot.ppm"):
    print("OK shot size:", os.path.getsize("/tmp/shot.ppm"))
else:
    print("FAIL: no screenshot")