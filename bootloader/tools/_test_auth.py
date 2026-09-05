#!/usr/bin/env python3
# MiniOS user/permission/sudo E2E test via QEMU monitor sendkey.
# Usage: python3 tools/_test_auth.py   (run inside WSL at /mnt/d/MyOS/bootloader)
import subprocess, socket, time, os, sys

os.system("rm -f /tmp/mon.sock /tmp/shot.ppm /tmp/ser.txt")

qemu = [
    "qemu-system-x86_64", "-cdrom", "build/os.iso", "-boot", "d",
    "-m", "2G", "-display", "none",
    "-monitor", "unix:/tmp/mon.sock,server,nowait",
    "-serial", "file:/tmp/ser.txt", "-no-reboot",
]
p = subprocess.Popen(qemu, stdout=open("/dev/null", "w"), stderr=subprocess.STDOUT)

time.sleep(24)   # let kernel boot to login prompt

s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
for i in range(20):
    try:
        s.connect("/tmp/mon.sock"); break
    except Exception:
        time.sleep(1)
else:
    print("MONITOR CONNECT FAIL"); p.kill(); sys.exit(1)

def keys(text, wait=0.05):
    for ch in text:
        name = {" ": "spc", ".": "dot", "/": "slash", "-": "minus",
                "_": "shift-minus", "*": "shift-8"}.get(ch, ch)
        s.sendall(("sendkey %s\n" % name).encode())
        time.sleep(wait)

def cmd(text, wait=1.5):
    keys(text)
    s.sendall(b"sendkey ret\n")
    time.sleep(wait)

def dump(tag):
    s.sendall(b"screendump /tmp/shot.ppm\n")
    time.sleep(1.5)
    if os.path.exists("/tmp/shot.ppm"):
        os.system("cp /tmp/shot.ppm /mnt/d/MyOS/win11-desktop/auth_%s.ppm" % tag)

print("== login as root ==")
cmd("root", 1.0)
cmd("admin", 1.5)          # password
dump("root_login")

print("== basic id ==")
cmd("whoami", 1.0)
cmd("id", 1.0)
cmd("users", 1.0)
dump("id")

print("== create + protect file ==")
cmd("touch secret.txt", 1.0)
cmd("chmod 600 secret.txt", 1.0)
cmd("stat secret.txt", 1.5)
dump("stat")

print("== add bob ==")
cmd("useradd bob bob123", 1.5)
cmd("users", 1.0)
dump("users")

print("== switch to bob ==")
cmd("logout", 1.5)
cmd("bob", 1.0)
cmd("bob123", 1.5)         # password
dump("bob_login")

print("== bob denied read (600 root-owned) ==")
cmd("whoami", 1.0)
cmd("cat secret.txt", 1.5)
dump("denied")

print("== bob sudo reads it ==")
cmd("sudo cat secret.txt", 1.0)
cmd("bob123", 2.0)         # sudo password
dump("sudo")

print("== bob cannot deluser root ==")
cmd("deluser root", 1.5)
dump("deluser")

print("== back to root ==")
cmd("logout", 1.5)
cmd("root", 1.0)
cmd("admin", 1.5)
dump("root_back")

s.sendall(b"quit\n")
s.close()
p.wait(timeout=10)
print("DONE")
