#!/usr/bin/env python3
"""Final test: OVMF + -m 4G + sendkey 'run test.bat' + screendump."""
import subprocess, socket, time, os, sys

os.system("rm -f /tmp/mon.sock /tmp/ovmf_vars.fd /tmp/shot.ppm")
ovmf_code = "/usr/share/OVMF/OVMF_CODE_4M.fd"
ovmf_vars = "/tmp/ovmf_vars.fd"
subprocess.run(["cp", "/usr/share/OVMF/OVMF_VARS_4M.fd", ovmf_vars], check=True)

p = subprocess.Popen(
    ["qemu-system-x86_64",
     "-cdrom","build/os.iso",
     "-drive","if=pflash,format=raw,readonly=on,file="+ovmf_code,
     "-drive","if=pflash,format=raw,file="+ovmf_vars,
     "-m","4G","-display","none",
     "-monitor","unix:/tmp/mon.sock,server,nowait",
     "-serial","file:/tmp/ser.txt","-no-reboot"],
    stdout=open("/dev/null"), stderr=subprocess.STDOUT)

# OVMF boot is slow; wait long enough
print("Waiting for kernel to boot (60s)...", flush=True)
time.sleep(60)
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