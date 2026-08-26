#!/usr/bin/env python3
"""VM interactive test of the right-click fixes + button press animation.
Boots build/os.img (BIOS, managed Win11 shell), switches to the default
desktop (Ctrl+Left), then drives QEMU HMP mouse events to exercise:
  1. desktop icon right-click -> Rename (inline editor + typing)
  2. native File Explorer right-click -> native menu -> Properties
  3. (optional) a managed button press animation frame
Each step dumps a PPM/PNG so we can inspect what actually happened."""
import socket, time, subprocess, os, sys

PROJ = r"D:\MyOS\bootloader"
BUILD = os.path.join(PROJ, "build")
QEMU  = r"D:\qemu\qemu-system-x86_64.exe"
DISK  = os.path.join(BUILD, "os.img")
SERIAL = os.path.join(BUILD, "serial_ctx.txt")
MON  = 5597
WAIT = int(sys.argv[1]) if len(sys.argv) > 1 else 40

for f in [SERIAL]:
    if os.path.exists(f): os.remove(f)

qemu_err = os.path.join(BUILD, "qemu_ctx_err.txt")
cmd = [QEMU, "-accel", "tcg", "-drive", f"format=raw,file={DISK}",
       "-m", "4096", "-vga", "std", "-display", "none",
       "-serial", f"file:{SERIAL}",
       "-monitor", f"tcp:127.0.0.1:{MON},server,nowait",
       "-net", "nic,model=ne2k_isa", "-net", "user"]
p = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=open(qemu_err, "w"))
s = None
for _ in range(30):
    time.sleep(1.0)
    try:
        s = socket.create_connection(("127.0.0.1", MON), timeout=5); break
    except (ConnectionRefusedError, OSError):
        if p.poll() is not None: break
if s is None:
    print("FATAL: monitor never came up"); sys.exit(1)
s.settimeout(1.5)

def read_line():
    buf = b""
    while True:
        try: d = s.recv(1)
        except socket.timeout: return buf.decode(errors="replace")
        if not d: return buf.decode(errors="replace")
        buf += d
        if d == b"\n": return buf.decode(errors="replace")

def cmdq(c, wait=0.35):
    s.sendall((c + "\n").encode()); time.sleep(wait)
    out = []
    while True:
        ln = read_line()
        if not ln: continue
        out.append(ln.rstrip("\n"))
        if ln.startswith("(qemu)"): break
        if len(out) > 6: break
    return out

def shot(name):
    path = os.path.join(BUILD, name)
    cmdq(f"screendump {path}", 0.5)
    return path

# wait for desktop
t0 = time.time(); entered = False
while time.time() - t0 < WAIT:
    time.sleep(1.0)
    try: txt = open(SERIAL, encoding="utf-8", errors="replace").read()
    except Exception: txt = ""
    if "Entered GUI mode" in txt: entered = True; break
time.sleep(3.0)
print("Entered GUI mode:", entered)

# switch to the default desktop (Ctrl+Left = -7 -> SwitchDesktop(0))
cmdq("sendkey ctrl-left")
time.sleep(1.5)

def mouse_move(x, y):
    # QEMU HMP mouse_move is RELATIVE; snap to the top-left origin first.
    cmdq("mouse_move -10000 -10000"); time.sleep(0.08)
    cmdq(f"mouse_move {x} {y}")
def left_click(x, y):
    mouse_move(x, y); time.sleep(0.25)
    cmdq("mouse_button 1"); time.sleep(0.12); cmdq("mouse_button 0"); time.sleep(0.3)
def right_click(x, y):
    mouse_move(x, y); time.sleep(0.25)
    cmdq("mouse_button 2"); time.sleep(0.12); cmdq("mouse_button 0"); time.sleep(0.3)

# screenshot to find the first desktop icon tile (colored squares on the
# default desktop grid: gridX=223, gridY=100, tile 92x66, gap 14)
p1 = shot("ctx_base.ppm")
print("base shot:", p1)

# ---- 1. desktop icon right-click -> menu -> Rename ----
# (icon #0 near (223,100); the menu opens at the click point)
icx, icy = 223 + 46, 100 + 33
right_click(icx, icy)
time.sleep(0.5)
p2 = shot("ctx_desktop_menu.ppm")
print("desktop menu shot:", p2)

# Rename is the 7th enabled item in OpenFileMenu
# (Open/Edit/Open-with-Terminal/Open-with.../sep/Copy/Delete/Rename)
# item height 34, top pad 6 -> y = icy+6+7*34
ry = icy + 6 + 7 * 34
left_click(icx + 40, ry)
time.sleep(0.5)
p3 = shot("ctx_rename.ppm")
print("rename editor shot:", p3)

# type a new name + Enter
cmdq('sendkey "n"'); cmdq('sendkey "e"'); cmdq('sendkey "w"')
cmdq('sendkey "1"')
time.sleep(0.3)
cmdq("sendkey ret")
time.sleep(0.6)
p4 = shot("ctx_renamed.ppm")
print("renamed shot:", p4)

s.sendall(b"quit\n")
try: p.wait(8)
except Exception: p.kill()
print("DONE")
