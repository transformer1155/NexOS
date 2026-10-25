#!/usr/bin/env python3
import socket, time, subprocess, os, sys, re

PROJ = r"D:\MyOS\bootloader"
BUILD = os.path.join(PROJ, "build")
QEMU = r"D:\qemu\qemu-system-x86_64.exe"
OVMF = r"D:\qemu\share\edk2-x86_64-code.fd"
DISK = os.path.join(BUILD, "os_uefi_highfb_test.img")
SERIAL = os.path.join(BUILD, "serial_verify_highfb.txt")
MON = 5592
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
       "-display", "none", "-vga", "none", "-device", "ramfb,id=rfb"]

if os.path.exists(SERIAL):
    os.remove(SERIAL)
qemu_err = os.path.join(BUILD, "qemu_verify_err.txt")
p = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=open(qemu_err, "w"))
# wait for the monitor socket to come up (QEMU cold-start can be slow)
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
    print("FATAL: monitor socket never came up. QEMU stderr:")
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

def wait_prompt():
    # read until we see the (qemu) prompt or a dump line
    lines = []
    while True:
        ln = read_line()
        if ln == "":
            continue
        if ln.startswith("(qemu)"):
            break
        lines.append(ln)
        if len(lines) > 40:
            break
    return lines

def xp(addr, n=16):
    # drain any pending output, then send one command and read until prompt
    drain_leftover()
    s.sendall((f"xp /{n}xw {addr}\n").encode())
    time.sleep(0.5)
    out = []
    while True:
        ln = read_line()
        if ln == "":
            continue
        st = ln.strip()
        if st.startswith("(qemu)"):
            break
        if re.match(r"^[0-9a-f]+:", st):
            out.append(st)
        if len(out) >= n // 4 and st.startswith("(qemu)"):
            break
    return out

def drain_leftover():
    s.settimeout(0.4)
    try:
        while True:
            d = s.recv(65536)
            if not d:
                break
    except socket.timeout:
        pass
    except Exception:
        pass
    s.settimeout(2.0)

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

time.sleep(4.0)  # settle

regions = {
    "row0_topbar  @0x100000000 (real FB)": "0x100000000",
    "row100_wall  @0x10004E200 (real FB)": "0x10004E200",
    "row300_wall  @0x1000F4240 (real FB)": "0x1000F4240",
    "row0_winfb   @0xF0000000 (window)   ": "0xF0000000",
}
for name, addr in regions.items():
    res = xp(addr, 16)
    print(f"=== {name} ===")
    if res:
        for ln in res:
            print("  " + ln[:80])
    else:
        print("  (no dump line returned)")

try:
    with open(SERIAL) as f:
        txt = f.read()
except Exception:
    txt = ""
print("=== serial tail ===")
for ln in txt.splitlines()[-25:]:
    print("  " + ln)
print("=== RESULT ===")
print("Entered GUI mode:", entered)

s.sendall(b"quit\n")
try:
    p.wait(8)
except Exception:
    p.kill()
