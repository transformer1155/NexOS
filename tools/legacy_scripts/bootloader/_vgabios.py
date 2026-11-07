#!/usr/bin/env python3
"""Search vgabios ROM for VESA signature."""
import subprocess, socket, time, re, os

os.system("rm -f /tmp/mon.sock")
p = subprocess.Popen(
    ["qemu-system-x86_64","-cdrom","build/os.iso","-boot","d",
     "-m","4G","-display","none",
     "-monitor","unix:/tmp/mon.sock,server,nowait",
     "-serial","file:/tmp/ser.txt","-no-reboot"],
    stdout=open("/dev/null"), stderr=subprocess.STDOUT)

time.sleep(28)
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
for i in range(20):
    try: s.connect("/tmp/mon.sock"); break
    except: time.sleep(1)

# Read 0xC0000-0xCFFFF (64KB vgabios)
s.sendall(b"x /65536bx 0xC0000\n"); time.sleep(3)
r = s.recv(1048576).decode(errors='ignore')

# Each x cmd output is "ADDR: byte byte byte ...\n". But this is 65536 bytes
# which is huge. Just search for VESA / VBE substrings.
for keyword in ["VESA", "VBE", "4F00", "0x4f", "4f02"]:
    cnt = r.count(keyword)
    print(f"  '{keyword}' occurrences in output: {cnt}")

# Also try to look at first 0x100 bytes of 0xC0000
s.sendall(b"x /256bx 0xC0000\n"); time.sleep(1)
r = s.recv(8192).decode(errors='ignore')
print("\n0xC0000-0xC0100 (first 256 bytes of vgabios ROM):")
for line in r.split("\n"):
    if "0x" in line and ":" in line:
        print(" ", line.strip()[:120])

s.sendall(b"quit\n"); s.close()
p.wait(timeout=10)