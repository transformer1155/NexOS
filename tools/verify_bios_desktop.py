#!/usr/bin/env python3
"""One-shot headless capture of the BIOS os.img desktop (managed Win11 shell).
Reuses the monitor+screendump recipe from tools/verify_screendump.py but
against build/os.img (BIOS raw) instead of the UEFI high-FB test image.
Outputs PPM + PNG into build/, prints color-bucket analysis."""
import socket, time, subprocess, os, sys

PROJ = r"D:\MyOS\bootloader"
BUILD = os.path.join(PROJ, "build")
QEMU  = r"D:\qemu\qemu-system-x86_64.exe"
DISK  = os.path.join(BUILD, "os.img")
SERIAL = os.path.join(BUILD, "serial_bios_desktop.txt")
PPM  = os.path.join(BUILD, "bios_desktop.ppm")
PNG  = os.path.join(BUILD, "bios_desktop.png")
MON  = 5594   # fresh port (avoids TIME_WAIT from previous QEMUs)
WAIT = int(sys.argv[1]) if len(sys.argv) > 1 else 45

for p in (SERIAL, PPM, PNG):
    if os.path.exists(p):
        os.remove(p)

qemu_err = os.path.join(BUILD, "qemu_bios_desktop_err.txt")
cmd = [QEMU, "-accel", "tcg",
       "-drive", f"format=raw,file={DISK}",
       "-m", "4096",
       "-vga", "std",
       "-display", "none",
       "-serial", f"file:{SERIAL}",
       "-monitor", f"tcp:127.0.0.1:{MON},server,nowait",
       "-net", "nic,model=ne2k_isa", "-net", "user"]

p = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=open(qemu_err, "w"))
s = None
for _ in range(30):
    time.sleep(1.0)
    try:
        s = socket.create_connection(("127.0.0.1", MON), timeout=5)
        break
    except (ConnectionRefusedError, OSError):
        if p.poll() is not None:
            break
if s is None:
    print("FATAL: monitor never came up")
    try:
        with open(qemu_err) as f:
            print(f.read()[:2000])
    except Exception:
        pass
    sys.exit(1)
s.settimeout(2.0)

def read_line():
    buf = b""
    while True:
        try:
            d = s.recv(1)
        except socket.timeout:
            return buf.decode(errors="replace")
        if not d:
            return buf.decode(errors="replace")
        buf += d
        if d == b"\n":
            return buf.decode(errors="replace")

def cmdq(c):
    s.sendall((c + "\n").encode())
    time.sleep(0.6)
    out = []
    while True:
        ln = read_line()
        if not ln:
            continue
        out.append(ln.rstrip("\n"))
        if ln.startswith("(qemu)"):
            break
        if len(out) > 10:
            break
    return out

# Wait for the desktop to come up: "Entered GUI mode" is the last log line
# in enter_gui(), after render_all() has painted at least once.
t0 = time.time()
entered = False
while time.time() - t0 < WAIT:
    time.sleep(1.0)
    try:
        with open(SERIAL) as f:
            txt = f.read()
    except Exception:
        txt = ""
    if "Entered GUI mode" in txt:
        entered = True
        break

# Let the managed shell paint a few more frames (icons / taskbar / clock)
time.sleep(4.0)

res = cmdq(f"screendump {PPM}")
print("screendump:", res)
print("Entered GUI mode:", entered)

# PPM -> PNG via Pillow
try:
    from PIL import Image
    if os.path.exists(PPM):
        Image.open(PPM).save(PNG)
        print(f"saved PNG: {PNG}")
    else:
        print("PPM not created")
except ImportError:
    print("Pillow not available; PPM left at", PPM)

# Quick colour analysis (same recipe as verify_screendump.py)
def analyze(path):
    if not os.path.exists(path):
        return "PPM NOT CREATED"
    with open(path, "rb") as f:
        data = f.read()
    if data[:2] != b"P6":
        return f"not P6: {data[:20]}"
    idx = 2
    def tok():
        nonlocal idx
        while idx < len(data) and data[idx] in b" \t\n\r":
            idx += 1
        s = idx
        while idx < len(data) and data[idx] not in b" \t\n\r":
            idx += 1
        return data[s:idx]
    w = int(tok()); h = int(tok()); mv = int(tok()); idx += 1
    px = data[idx:]
    colors = {}
    n = w * h; step = max(1, n // 4000)
    for i in range(0, n, step):
        o = i * 3
        if o + 3 > len(px): break
        r, g, b = px[o], px[o+1], px[o+2]
        k = (r // 8, g // 8, b // 8)
        colors[k] = colors.get(k, 0) + 1
    top = sorted(colors.items(), key=lambda kv: -kv[1])[:8]
    total = sum(colors.values())
    def near(c, t, tol=20):
        return all(abs(a-b) <= tol for a, b in zip(c, t))
    gray = sum(v for k, v in colors.items() if near((k[0]*8,k[1]*8,k[2]*8),(140,140,140)))
    darkbar = sum(v for k, v in colors.items() if (k[0]*8)<0x40 and (k[1]*8)<0x40 and (k[2]*8)<0x45 and abs(k[0]-k[1])<=2 and abs(k[1]-k[2])<=2)
    blue = sum(v for k, v in colors.items() if (k[2]*8) > (k[0]*8)+30 and (k[2]*8) > (k[1]*8)+20)
    return (f"{w}x{h} distinct={len(colors)} "
            f"gray140={gray/total:.2%} darkbar={darkbar/total:.2%} blue={blue/total:.2%} "
            f"top={[((k[0]*8,k[1]*8,k[2]*8),v) for k,v in top]}")

print("=== PPM analyze ===")
print(analyze(PPM))

s.sendall(b"quit\n")
try:
    p.wait(8)
except Exception:
    p.kill()
