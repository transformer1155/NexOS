p = r"d:\MyOS\bootloader\build\smoke_serial.log"
import os, time
print("now", time.time())
if os.path.exists(p):
    t = open(p, encoding="latin-1", errors="replace").read()
    print("load lines:", t.count("[load"))
    print("model loaded:", "model loaded" in t)
    print("inference done:", "inference done" in t)
    for l in t.splitlines():
        if "[load" in l or "model loaded" in l or "inference" in l or "AUTOTEST" in l:
            print(repr(l))
