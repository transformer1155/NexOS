#!/usr/bin/env python3
# Boot check with -device loader (0x501E=1) -> 64-bit TEXT mode. Capture serial
# to a file; report whether 64-bit boot markers + model load appear.
import os, sys, subprocess, time, signal

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IMG  = os.path.join(ROOT, "build", "os.img")
QEMU = r"D:\qemu\qemu-system-x86_64.exe"
SER  = os.path.join(ROOT, "build", "smoke_serial.log")
open(SER, "w").close()

CMD = [QEMU, "-m","2G","-accel","tcg","-drive","file=%s,format=raw,if=ide"%IMG,
       "-device","loader,addr=0x501E,data=1,data-len=1",
       "-serial","file:%s"%SER,
       "-monitor","none","-display","none","-no-reboot"]

print("[launch]", " ".join(CMD))
p = subprocess.Popen(CMD, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
print("[pid]", p.pid)
for t in range(8):
    time.sleep(8)
    with open(SER, "r", errors="replace") as f:
        txt = f.read()
    print("--- t=%ds serial len=%d ---" % (8*(t+1), len(txt)))
    if "64" in txt or "qwen" in txt.lower() or "model" in txt.lower() or "shell" in txt.lower():
        print(txt[-2000:])
        break
else:
    with open(SER, "r", errors="replace") as f:
        print("FINAL serial:\n", f.read()[-2500:])
try: p.send_signal(signal.SIGTERM)
except Exception: pass
print("[done]")
