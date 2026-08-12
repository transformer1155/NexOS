#!/usr/bin/env python3
"""Long wait OVMF test."""
import subprocess, socket, time, os

os.system("rm -f /tmp/mon.sock /tmp/ovmf_vars.fd /tmp/shot.ppm /tmp/ser.txt")
subprocess.run(["cp","/usr/share/OVMF/OVMF_VARS_4M.fd","/tmp/ovmf_vars.fd"], check=True)

p = subprocess.Popen(
    ["qemu-system-x86_64","-cdrom","build/os.iso",
     "-drive","if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd",
     "-drive","if=pflash,format=raw,file=/tmp/ovmf_vars.fd",
     "-m","4G","-display","none",
     "-monitor","unix:/tmp/mon.sock,server,nowait",
     "-serial","file:/tmp/ser.txt","-no-reboot"],
    stdout=open("/dev/null"), stderr=subprocess.STDOUT)

print("Sleep 80s for OVMF + bootuefi + kernel boot...", flush=True)
time.sleep(80)

# Check serial to see if kernel booted
ser = open("/tmp/ser.txt","rb").read().decode(errors='ignore')
if "[K1]" in ser:
    print("kernel booted OK")
else:
    print("kernel NOT yet booted; serial tail:")
    print(ser[-400:])

s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
for i in range(20):
    try: s.connect("/tmp/mon.sock"); break
    except: time.sleep(1)

for k in "run test.bat":
    name = {" ": "spc", ".": "dot"}.get(k, k)
    s.sendall(f"sendkey {name}\n".encode()); time.sleep(0.05)
s.sendall(b"sendkey ret\n")
time.sleep(15)
s.sendall(b"screendump /tmp/shot.ppm\n"); time.sleep(2)
s.sendall(b"quit\n"); s.close()
p.wait(timeout=15)

if os.path.exists("/tmp/shot.ppm"):
    print("shot size:", os.path.getsize("/tmp/shot.ppm"))
    with open("/tmp/shot.ppm","rb") as f: d = f.read()
    import re
    m = re.search(rb"(\d+) x (\d+)", d[:200])
    if m: print(f"res: {m.group(1).decode()}x{m.group(2).decode()}")