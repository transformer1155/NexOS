#!/usr/bin/env python3
"""Comprehensive NexOS feature test via WSL+QEMU.
Boots os.img, logs in, tests all major feature categories, captures serial.
Usage (in WSL): python3 tools/wsl_feature_test.py [image]
"""
import subprocess, socket, time, os, sys

IMG = sys.argv[1] if len(sys.argv) > 1 else "build/os.img"
TOOLS = "/mnt/d/MyOS/bootloader/tools"
BUILD = "/mnt/d/MyOS/bootloader/build"
MON_SOCK = "/tmp/feat_mon.sock"
SERIAL = "/mnt/d/MyOS/bootloader/build/feat_serial.txt"
LOG = f"{BUILD}/feat_report.txt"

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
    try: s.connect(MON_SOCK); connected = True; break
    except: time.sleep(1)
if not connected:
    print("MONITOR CONNECT FAIL"); p.kill(); sys.exit(1)
s.settimeout(2.0)
try: s.recv(4096)
except: pass

def send_cmd(cmd):
    s.sendall((cmd + "\n").encode())
    time.sleep(0.12)
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
    ',': 'comma', '=': 'equal', '*': 'shift-8',
    "'": 'apostrophe', '"': 'shift-quote', ';': 'semicolon',
    '<': 'shift-comma', '>': 'shift-dot', '?': 'shift-slash',
    '%': 'shift-5', '$': 'shift-4', '&': 'shift-7', '+': 'shift-equal',
    '[': 'bracket_left', ']': 'bracket_right', '{': 'shift-bracket_left',
    '}': 'shift-bracket_right', '^': 'shift-6', '~': 'shift-backquote',
    '|': 'shift-backslash', '`': 'backquote',
}
for d in "0123456789":
    KEYMAP[d] = d

def type_line(text, delay=0.15):
    for ch in text:
        if ch.isupper():
            sendkey(f"shift-{ch.lower()}")
        else:
            sendkey(KEYMAP.get(ch, ch))
    sendkey("ret")
    time.sleep(delay)

# ===== TEST PLAN =====
# Each entry: (category, command, expected_serial_fragment_or_None)
TESTS = [
    # --- Login ---
    ("login_user",  "root",       None),
    ("login_pass",  "admin",      None),

    # --- Core shell ---
    ("help",        "help",       None),
    ("echo",        "echo hello_NexOS", None),
    ("whoami",      "whoami",     None),
    ("about",       "about",      None),
    ("ls_root",     "ls",         None),
    ("pwd",         "pwd",        None),
    ("hostname",    "hostname",   None),
    ("date",        "date",       None),
    ("uptime",      "uptime",     None),
    ("clear",       "clear",      None),

    # --- Users & permissions ---
    ("useradd",     "useradd testuser", None),
    ("passwd_chg",  "passwd testuser newpass123", None),
    ("sudo_test",   "sudo whoami", None),
    ("id_cmd",      "id",         None),

    # --- Filesystem ---
    ("mkdir_test",  "mkdir /tmp", None),
    ("touch_test",  "touch /tmp/hello.txt", None),
    ("write_test",  "echo hello_world > /tmp/hello.txt", None),
    ("cat_test",    "cat /tmp/hello.txt", None),
    ("ls_tmp",      "ls /tmp",     None),
    ("cp_test",     "cp /tmp/hello.txt /tmp/copy.txt", None),
    ("mv_test",     "mv /tmp/copy.txt /tmp/moved.txt", None),
    ("rm_test",     "rm /tmp/moved.txt", None),

    # --- Disk/FS ---
    ("mkfs_warn",   "mkfs",       None),  # might need confirmation
    ("stat_test",   "stat /",     None),
    ("df_test",     "df",         None),

    # --- Network ---
    ("net_status",  "net status", None),

    # --- System info ---
    ("meminfo",     "meminfo",    None),
    ("ps_test",     "ps",         None),
    ("env_test",    "env",        None),

    # --- Advanced ---
    ("ai_test",     "ai say hi",  None),
    ("script_test", "run /etc/motd.sh", None),  # if exists
    ("gui_test",    "gui",        None),  # start GUI if available
]

print("==> Waiting for boot (22s)...")
time.sleep(22)

results = {}
for item in TESTS:
    label = item[0]
    cmd = item[1]
    if cmd is None:
        continue
    print(f"==> [{label}] {cmd}")
    type_line(cmd, 0.8 if label in ("help","about","ls_root","ls_tmp","net_status","meminfo","ps_test","env_test") else 0.5)
    results[label] = "sent"

# cleanup
time.sleep(2)
s.sendall(b"quit\n"); s.close()
try: p.wait(timeout=10)
except: p.kill()

# Analyze serial
ser = open(SERIAL, 'r', errors='replace').read() if os.path.exists(SERIAL) else ""
lines = ser.strip().split('\n')

with open(LOG, 'w') as f:
    f.write("=" * 60 + "\n")
    f.write("NexOS Feature Test Report\n")
    f.write("=" * 60 + "\n\n")
    f.write(f"Total serial lines: {len(lines)}\n")
    f.write(f"Image: {IMG}\n\n")

    # Check key milestones
    checks = {
        "Boot": any("[K1] kmain entered" in l for l in lines),
        "VBE Graphics": any("VBE graphics mode active" in l for l in lines),
        "Network Init": any("Network initialized successfully" in l for l in lines),
        "Users Seeded": any("user0=root" in l or "seed_default" in l.lower() for l in lines),
        "Login Success": any("Welcome" in l for l in lines),
        "Shell Started": any("[SHELL] $" in l for l in lines),
    }
    f.write("--- Milestones ---\n")
    for name, ok in checks.items():
        f.write(f"  {'PASS' if ok else 'FAIL'}: {name}\n")
    f.write("\n")

    # Count commands received
    shell_cmds = [l for l in lines if "[SHELL] $" in l]
    f.write(f"--- Shell Commands Received: {len(shell_cmds)} ---\n")
    for c in shell_cmds:
        f.write(f"  {c}\n")
    f.write("\n")

    # Full serial (last 80 lines for context)
    f.write("--- Serial (last 80 lines) ---\n")
    for l in lines[-80:]:
        f.write(f"  {l}\n")

print(f"\n{'='*50}")
print(f"Milestones: {sum(checks.values())}/{len(checks)} passed")
print(f"Shell commands received: {len(shell_cmds)}")
print(f"Full report: {LOG}")
print(f"Serial: {SERIAL} ({len(lines)} lines)")
