#!/usr/bin/env python3
"""Test if SeaBIOS VBE works in QEMU."""
import subprocess, socket, time, os, sys

os.system("rm -f /tmp/mon.sock /tmp/shot.ppm /tmp/vbe.bin /tmp/c0000.bin")
p = subprocess.Popen(
    ["qemu-system-x86_64","-cdrom","build/os.iso","-boot","d",
     "-m","4G","-display","none",
     "-monitor","unix:/tmp/mon.sock,server,nowait",
     "-serial","file:/tmp/ser.txt","-no-reboot"],
    stdout=open("/dev/null"), stderr=subprocess.STDOUT)

# Wait long enough for boot_cd to run vbe_query_only (which runs in stage2 right after kernel load)
time.sleep(28)
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
for i in range(20):
    try: s.connect("/tmp/mon.sock"); break
    except: time.sleep(1)

for addr in [0x5000, 0x5400, 0xC0000]:
    s.sendall(f"x /32bx 0x{addr:x}\n".encode()); time.sleep(1)
    r = s.recv(2048).decode(errors='ignore')
    print(f"\n=== 0x{addr:x} ===")
    for line in r.split("\n"):
        if "0x" in line and ":" in line and ("0x{:08x}".format(addr) in line or "0000000" in line):
            print(" ", line.strip()[:120])

# Also try: ask QEMU to run an INT 10h from inside the VM
# (But that requires the kernel to be cooperative; we just probe memory.)
s.sendall(b"quit\n"); s.close()
p.wait(timeout=10)
print("\nserial tail:", open("/tmp/ser.txt","rb").read()[-200:].decode(errors='ignore'))