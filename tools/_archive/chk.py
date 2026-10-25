import time, os
print("now", time.time())
p = r"d:\MyOS\bootloader\build\smoke_serial.log"
if os.path.exists(p):
    st = os.stat(p)
    print("serial size", st.st_size, "mtime", st.st_mtime)
    t = open(p, encoding="latin-1", errors="replace").read()
    print("AUTOTEST", "AUTOTEST" in t)
    print("model loaded", "model loaded" in t)
    print("FAILED", "FAILED" in t)
    for l in t.splitlines():
        if "AUTOTEST" in l or "model" in l.lower() or "DeepSeek" in l or "done" in l.lower():
            print(repr(l))
