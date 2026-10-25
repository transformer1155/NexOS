#!/usr/bin/env python3
import socket, time, subprocess
ISO = "/mnt/d/MyOS/bootloader/build/os.iso"
PORT = 4512
def wait(port, t=30.0):
    end=time.time()+t
    while time.time()<end:
        try: return socket.create_connection(("127.0.0.1",port),0.5)
        except OSError: time.sleep(0.2)
    raise RuntimeError("no monitor")
def cmd(mon, c, settle=0.6):
    mon.sendall((c+"\n").encode())
    time.sleep(settle)
    mon.settimeout(1.5)
    buf=b""
    try:
        while True:
            d=mon.recv(4096)
            if not d: break
            buf+=d
            if buf.rfind(b"(qemu)") > buf.find(b"(qemu)"): break
    except socket.timeout: pass
    return buf.decode("latin-1","ignore")
q=subprocess.Popen(["qemu-system-x86_64","-cdrom",ISO,"-boot","d","-m","128M","-vga","std",
    "-display","none","-no-reboot","-serial","file:/tmp/serH.log",
    "-monitor",f"tcp:127.0.0.1:{PORT},server,nowait"])
mon=wait(PORT); mon.settimeout(2.0)
try: mon.recv(65536)
except Exception: pass
time.sleep(3.0)
for c in ["info registers","xp /8xw 0x7f90","xp /8xw 0x7eb9"]:
    print("==== "+c+" ====")
    print(cmd(mon,c))
mon.sendall(b"quit\n"); time.sleep(0.5)
try: q.wait(timeout=3)
except Exception: q.terminate()
