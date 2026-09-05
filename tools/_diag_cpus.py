#!/usr/bin/env python3
"""Boot, switch to 64-bit, run smp_init, then query QEMU monitor 'info cpus'
to see how many CPUs QEMU actually has and their state."""
import os, socket, subprocess, sys, time
try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)
import glob as _g
IMG = sorted(_g.glob("build/os_textboot_qemu_*.img"))[-1]
LOG = "build/k64_diag.log"; ERR="build/k64_diag.err"; MON=4481
QEMU="D:/qemu/qemu-system-x86_64.exe"

def wait_sock(p, t=40.0):
    e=time.time()+t
    while time.time()<e:
        try: return socket.create_connection(("127.0.0.1",p),1.0)
        except OSError: time.sleep(0.25)
    raise RuntimeError("no mon")
def ser():
    try:
        with open(LOG,"r",encoding="latin-1",errors="replace") as f: return f.read()
    except: return ""
def wf(n,t=60.0):
    e=time.time()+t
    while time.time()<e:
        if n in ser(): return True
        time.sleep(0.3)
    return False
def tline(m,s):
    for ch in s:
        m.sendall(("sendkey %s\n"%ch).encode()); time.sleep(0.05)
    m.sendall(b"sendkey ret\n"); time.sleep(0.4)
def qmon(m,cmd):
    try:
        m.sendall((cmd+"\n").encode())
        time.sleep(1.2)
        m.settimeout(3.0)
        data=b""
        try:
            while True:
                chunk=m.recv(65536)
                if not chunk: break
                data+=chunk
        except socket.timeout:
            pass
        return data.decode("latin-1","replace")
    except Exception as ex:
        return "ERR:%s"%ex

errf=open(ERR,"wb")
proc=subprocess.Popen([QEMU,"-m","1024","-smp","4","-accel","tcg,tb-size=128",
    "-display","none","-no-reboot","-monitor","tcp:127.0.0.1:%d,server,nowait"%MON,
    "-serial","file:%s"%LOG,"-drive","format=raw,file=%s"%IMG],
    stdout=errf,stderr=errf)
try:
    mon=wait_sock(MON)
    wf("[K5] Hello world written",90.0)
    time.sleep(1.0)
    if wf("login:",8.0):
        tline(mon,"root"); time.sleep(0.6); tline(mon,"admin")
    else:
        tline(mon,"root"); time.sleep(0.6); tline(mon,"admin")
    time.sleep(1.0)
    tline(mon,"switch")
    wf("[K64-1] kmain64 entered",40.0)
    wf("[SMP] init",30.0)
    time.sleep(2.0)
    print("=== info cpus ===")
    print(qmon(mon,"info cpus"))
    print("=== info status ===")
    print(qmon(mon,"info status"))
    mon.sendall(b"quit\n"); time.sleep(1.0)
finally:
    try: proc.wait(timeout=6)
    except Exception: proc.terminate(); proc.kill()
    errf.close()
print("=== serial tail ===")
for ln in ser().splitlines()[-25:]:
    print(repr(ln))
