#!/usr/bin/env python3
import socket, time, subprocess, os, sys, struct, re

PROJ = r"D:\MyOS\bootloader"
BUILD = os.path.join(PROJ, "build")
QEMU = r"D:\qemu\qemu-system-x86_64.exe"
OVMF = r"D:\qemu\share\edk2-x86_64-code.fd"
DISK = os.path.join(BUILD, "os_uefi_highfb_test.img")
SERIAL = os.path.join(BUILD, "serial_verify_highfb.txt")
PPM = os.path.join(BUILD, "verify_screendump.ppm")
MON = 5593
WAIT = int(sys.argv[1]) if len(sys.argv) > 1 else 75

vars_src = os.path.join(BUILD, "ovmf_vars_test.fd")
if not os.path.exists(vars_src):
    open(vars_src, "wb").close()

cmd = [QEMU, "-machine", "q35", "-m", "5G", "-accel", "tcg",
       "-drive", f"if=pflash,format=raw,readonly=on,file={OVMF}",
       "-drive", f"if=pflash,format=raw,file={vars_src}",
       "-drive", f"file={DISK},format=raw,if=ide",
       "-serial", f"file:{SERIAL}",
       "-monitor", f"tcp:127.0.0.1:{MON},server,nowait",
       "-display", "vnc=127.0.0.1:0", "-vga", "none", "-device", "ramfb,id=rfb"]

if os.path.exists(SERIAL):
    os.remove(SERIAL)
if os.path.exists(PPM):
    os.remove(PPM)

qemu_err = os.path.join(BUILD, "qemu_verify_err.txt")
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

def cmd(c):
    s.sendall((c + "\n").encode())
    time.sleep(0.6)
    lines = []
    while True:
        ln = read_line()
        if ln == "":
            continue
        lines.append(ln.rstrip("\n"))
        if ln.startswith("(qemu)"):
            break
        if len(lines) > 10:
            break
    return lines

# wait for Entered GUI mode
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

time.sleep(4.0)
res = cmd(f"screendump {PPM}")
print("screendump response:", res)
print("Entered GUI mode:", entered)

# analyze PPM
def analyze(path):
    if not os.path.exists(path):
        return "PPM NOT CREATED"
    with open(path, "rb") as f:
        data = f.read()
    # parse PPM 'P6' header
    if data[:2] != b"P6":
        return f"not P6: {data[:20]}"
    idx = 2
    # skip whitespace
    def next_token():
        nonlocal idx
        while idx < len(data) and data[idx] in b" \t\n\r":
            idx += 1
        start = idx
        while idx < len(data) and data[idx] not in b" \t\n\r":
            idx += 1
        return data[start:idx]
    w = int(next_token())
    h = int(next_token())
    maxv = int(next_token())
    # single whitespace after maxval
    idx += 1
    px = data[idx:]
    # sample grid
    colors = {}
    n = w * h
    step = max(1, n // 4000)
    for i in range(0, n, step):
        o = i * 3
        if o + 3 > len(px):
            break
        r, g, b = px[o], px[o + 1], px[o + 2]
        key = (r // 8, g // 8, b // 8)
        colors[key] = colors.get(key, 0) + 1
    top = sorted(colors.items(), key=lambda kv: -kv[1])[:8]
    distinct = len(colors)
    # detect gray default (140,140,140) or pure black/uniform
    def near(c, t, tol=20):
        return all(abs(a - b) <= tol for a, b in zip(c, t))
    gray = sum(v for (r, g, b), v in colors.items() if near((r*8, g*8, b*8), (140, 140, 140)))
    blue = sum(v for (r, g, b), v in colors.items() if (b*8) > (r*8) + 30 and (b*8) > (g*8) + 20)
    total = sum(colors.values())
    return (f"{w}x{h} maxval={maxv}, distinct_color_buckets={distinct}, "
            f"gray140_share={gray/total:.2%}, blue_share={blue/total:.2%}, "
            f"top_buckets={[( (r*8,g*8,b*8),v) for (r,g,b),v in top]}")

print("=== PPM analyze ===")
print(analyze(PPM))

s.sendall(b"quit\n")
try:
    p.wait(8)
except Exception:
    p.kill()
