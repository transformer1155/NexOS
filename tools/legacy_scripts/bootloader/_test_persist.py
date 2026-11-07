#!/usr/bin/env python3
# Verify MKFS persistence: boot os.img as IDE HDD twice.
# 1st boot: mkfs + touch hello.txt + write content
# 2nd boot: ls should show hello.txt
import subprocess, socket, time, os, sys

def run_qemu(serial_file):
    os.system("rm -f /tmp/mon.sock")
    qemu = [
        "qemu-system-x86_64",
        "-fda", "build/os.img",
        "-boot", "order=a", "-m", "2G", "-display", "none",
        "-monitor", "unix:/tmp/mon.sock,server,nowait",
        "-serial", "file:" + serial_file, "-no-reboot",
    ]
    p = subprocess.Popen(qemu, stdout=open("/dev/null", "w"), stderr=subprocess.STDOUT)
    time.sleep(24)
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    for i in range(20):
        try:
            s.connect("/tmp/mon.sock"); break
        except Exception:
            time.sleep(1)
    else:
        print("MONITOR CONNECT FAIL"); p.kill(); sys.exit(1)
    return p, s

def keys(s, text, wait=0.05):
    for ch in text:
        name = {" ": "spc", ".": "dot", "/": "slash"}.get(ch, ch)
        s.sendall(("sendkey %s\n" % name).encode())
        time.sleep(wait)

def cmd(s, text, wait=1.5):
    keys(s, text)
    s.sendall(b"sendkey ret\n")
    time.sleep(wait)

def dump(s, tag):
    s.sendall(b"screendump /tmp/shot.ppm\n")
    time.sleep(1.5)
    if os.path.exists("/tmp/shot.ppm"):
        os.system("cp /tmp/shot.ppm /mnt/d/MyOS/win11-desktop/persist_%s.ppm" % tag)

print("== BOOT 1: format + create file ==")
p, s = run_qemu("/tmp/ser1.txt")
cmd(s, "root", 1.0)
cmd(s, "admin", 1.5)
cmd(s, "mkfs", 1.5)
cmd(s, "mkdir myfolder", 1.0)
cmd(s, "touch hello.txt", 1.0)
cmd(s, "ls", 1.5)
dump(s, "boot1")
s.sendall(b"quit\n"); s.close(); p.wait(timeout=10)
print("boot1 done")

print("== BOOT 2: file should persist ==")
p, s = run_qemu("/tmp/ser2.txt")
cmd(s, "root", 1.0)
cmd(s, "admin", 1.5)
cmd(s, "ls", 1.5)
dump(s, "boot2")
s.sendall(b"quit\n"); s.close(); p.wait(timeout=10)
print("boot2 done")
