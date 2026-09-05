#!/usr/bin/env python3
"""WSL framebuffer test harness for NexOS.
Boots os.img in QEMU (WSL), types commands, screendumps the VBE framebuffer
after each, converts PPM->PNG (pure python), and saves to build/ftest_*.png.
Also captures serial for boot diagnostics + crash detection.
Run inside WSL: python3 tools/wsl_ftest.py [image]
"""
import subprocess, socket, time, os, sys

IMG = sys.argv[1] if len(sys.argv) > 1 else "build/os.img"
TOOLS = "/mnt/d/MyOS/bootloader/tools"
BUILD = "/mnt/d/MyOS/bootloader/build"
MON_SOCK = "/tmp/NexOS_ft_mon.sock"
SERIAL = "/tmp/NexOS_ft_serial.txt"
LOG = f"{BUILD}/ftest_log.txt"

# (label, command) pairs. label is used for the png filename.
CMDS = [
    ("boot",   None),          # capture initial screen (login prompt)
    ("login_user",  "root"),       # username
    ("login_pass",  "admin"),      # password
    ("help",   "help"),
    ("echo",   "echo hello_world"),
    ("whoami", "whoami"),
    ("about",  "about"),
    ("ls",     "ls"),
]

os.system(f"rm -f {MON_SOCK} {SERIAL}")

qemu = [
    "qemu-system-x86_64",
    "-drive", f"format=raw,file={IMG}",
    "-m", "64M", "-display", "none", "-no-reboot",
    "-monitor", f"unix:{MON_SOCK},server,nowait",
    "-serial", f"file:{SERIAL}",
]
print(f"==> Starting QEMU: {IMG}")
p = subprocess.Popen(qemu, stdout=open("/dev/null", "w"), stderr=subprocess.STDOUT)

s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
connected = False
for i in range(20):
    try:
        s.connect(MON_SOCK); connected = True; break
    except Exception:
        time.sleep(1)
if not connected:
    print("MONITOR CONNECT FAIL"); p.kill(); sys.exit(1)
s.settimeout(2.0)
try: s.recv(4096)
except: pass

def send_cmd(cmd):
    s.sendall((cmd + "\n").encode())
    time.sleep(0.15)
    try: return s.recv(8192).decode(errors='replace')
    except socket.timeout: return ""

def sendkey(key, delay=0.04):
    send_cmd(f"sendkey {key}")
    time.sleep(delay)

KEYMAP = {
    ' ': 'spc', '.': 'dot', '/': 'slash', '\\': 'backslash',
    '-': 'minus', '_': 'shift-minus', ':': 'shift-semicolon',
    '!': 'shift-1', '@': 'shift-2', '#': 'shift-3',
    '(': 'shift-parenleft', ')': 'shift-parenright',
    ',': 'comma', '=': 'equal', '*': 'shift-8', '\n': 'ret',
    "'": 'apostrophe', '"': 'shift-quote', ';': 'semicolon',
    '<': 'shift-comma', '>': 'shift-dot', '?': 'shift-slash',
    '%': 'shift-5', '$': 'shift-4', '&': 'shift-7', '+': 'shift-equal',
    '[': 'bracket_left', ']': 'bracket_right', '{': 'shift-bracket_left',
    '}': 'shift-bracket_right', '^': 'shift-6', '~': 'shift-backquote',
    '|': 'shift-backslash', '`': 'backquote',
}
for d in "0123456789":
    KEYMAP[d] = d

def type_line(text, delay=0.12):
    for ch in text:
        if ch.isupper():
            sendkey(f"shift-{ch.lower()}")
        else:
            sendkey(KEYMAP.get(ch, ch))
    sendkey("ret")
    time.sleep(delay)

def shot(label):
    ppm = f"/tmp/ftest_{label}.ppm"
    png = f"{BUILD}/ftest_{label}.png"
    s.sendall(f"screendump {ppm}\n".encode())
    time.sleep(1.0)
    if os.path.exists(ppm):
        sz = os.path.getsize(ppm)
        # convert
        r = subprocess.run([sys.executable, f"{TOOLS}/ppm2png.py", ppm, png],
                           capture_output=True, text=True)
        print(f"  shot {label}: ppm={sz}B -> {png} ({r.returncode})")
    else:
        print(f"  shot {label}: NO PPM")

# boot wait - must be long enough for full init + login prompt
print("==> Waiting for boot (20s)...")
time.sleep(20)

for label, cmd in CMDS:
    if cmd is None:
        print(f"==> capture {label}")
    else:
        print(f"==> {label}: {cmd}")
        type_line(cmd, 0.9 if label in ("help","about","ls") else 0.6)
    shot(label)

# cleanup
s.sendall(b"quit\n"); s.close()
try: p.wait(timeout=10)
except: p.kill()

# serial
ser = open(SERIAL, 'r', errors='replace').read() if os.path.exists(SERIAL) else ""
with open(LOG, 'w') as f:
    f.write("=== SERIAL ===\n")
    f.write(ser if ser.strip() else "(empty)\n")
print(f"\nDone. serial lines={ser.count(chr(10))}, log={LOG}")
