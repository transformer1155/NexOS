#!/usr/bin/env python3
# Verify MKFS persistence on a dedicated data VHD (secondary ATA).
# Boot: os.iso (CD-ROM, read-only) + data.vhd (IDE index=1, writable)
import subprocess, socket, time, os, sys

def run_qemu(serial_file, data_vhd):
    os.system("rm -f /tmp/mon.sock")
    qemu = [
        "qemu-system-x86_64",
        "-cdrom", "build/os.iso", "-boot", "d", "-m", "2G",
        "-display", "none",
        "-monitor", "unix:/tmp/mon.sock,server,nowait",
        "-serial", "file:" + serial_file, "-no-reboot",
    ]
    if data_vhd:
        qemu += ["-drive", "file=%s,format=raw,if=ide,index=1" % data_vhd]
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
        os.system("cp /tmp/shot.ppm /mnt/d/MyOS/win11-desktop/dvhd_%s.ppm" % tag)

data = "build/data.vhd"

print("== BOOT 1: create file on data VHD ==")
p, s = run_qemu("/tmp/ser1.txt", data)
time.sleep(1)
cmd(s, "root", 1.0)
cmd(s, "admin", 1.5)
cmd(s, "mkdir myfolder", 1.0)
cmd(s, "touch hello.txt", 1.0)
cmd(s, "ls", 1.5)
dump(s, "boot1")
s.sendall(b"quit\n"); s.close(); p.wait(timeout=10)
print("boot1 done")

print("== BOOT 2: data VHD file should persist ==")
p, s = run_qemu("/tmp/ser2.txt", data)
cmd(s, "root", 1.0)
cmd(s, "admin", 1.5)
cmd(s, "ls", 1.5)
dump(s, "boot2")
s.sendall(b"quit\n"); s.close(); p.wait(timeout=10)
print("boot2 done")
