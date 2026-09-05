#!/usr/bin/env python3
"""Diagnostic: dump VBE info area and check current display mode."""
import subprocess, socket, time, os, sys

os.system("rm -f /tmp/mon.sock /tmp/shot.ppm /tmp/vbe.bin")
p = subprocess.Popen(
    ["qemu-system-x86_64","-cdrom","build/os.iso","-boot","d",
     "-m","4G","-display","none",
     "-monitor","unix:/tmp/mon.sock,server,nowait",
     "-serial","file:/tmp/ser.txt","-no-reboot"],
    stdout=open("/dev/null"), stderr=subprocess.STDOUT)

time.sleep(25)
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
for i in range(20):
    try: s.connect("/tmp/mon.sock"); break
    except: time.sleep(1)

s.sendall(b"x /32bx 0x5000\n"); time.sleep(1)
s.sendall(b"info registers\n"); time.sleep(1)
resp = s.recv(8192)
print("===REG===", resp[:600].decode(errors='ignore'))
if os.path.exists("/tmp/vbe.bin"):
    print("===VBE 0x5000 (32 bytes)===")
    print(open("/tmp/vbe.bin","rb").read().hex())

s.sendall(b"screendump /tmp/shot.ppm\n"); time.sleep(1)
s.sendall(b"quit\n"); s.close()
p.wait(timeout=10)
if os.path.exists("/tmp/shot.ppm"):
    sz = os.path.getsize("/tmp/shot.ppm")
    print("shot size:", sz, "(640x400=~768015, 1024x768x4=3145728)")