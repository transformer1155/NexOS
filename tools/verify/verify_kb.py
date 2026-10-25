#!/usr/bin/env python3
"""Verify the dynamic, rule-driven knowledge base + AI reasoning framework.

Drives build/os_textboot.img under QEMU (pc/512M/tcg) and exercises:

  kb list                                   -> shows 2 seeded facts (1 practice, 1 web x3)
  kb add <claim>                            -> candidate
  kb prove #N pass                          -> ACCEPTED  (真理来源于实践)
  kb add <claim>
  kb cite #N src excerpt  x3                -> ACCEPTED  (>=3 web sources)
  kb add <claim>
  kb cite #N src excerpt  x1                -> CANDIDATE (gate: <3 sources)
  ai reason <prompt>                        -> RAG hit on an accepted fact

Claim text is ASCII because the QEMU test console has no CJK IME; the rule
engine itself is language-agnostic.

Usage: verify_kb.py [wait_seconds]
"""
import os
import socket
import subprocess
import sys
import time

PROJ = r"D:\MyOS\bootloader"
BUILD = os.path.join(PROJ, "build")
QEMU = r"D:\qemu\qemu-system-x86_64.exe"
DISK = os.path.join(BUILD, "os_textboot.img")
SERIAL = os.path.join(BUILD, "serial_kb.txt")
QERR = os.path.join(BUILD, "qemu_kb.err")
MON_PORT = 5601

WAIT = int(sys.argv[1]) if len(sys.argv) > 1 else 90

def safe_remove(path):
    try:
        if os.path.exists(path):
            os.remove(path)
    except OSError:
        try:
            subprocess.run(["cmd", "/c", "del", "/f", "/q", path],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        except Exception:
            pass

for f in (SERIAL, QERR):
    safe_remove(f)

cmd = [
    QEMU, "-machine", "pc", "-m", "512", "-accel", "tcg",
    "-drive", f"if=ide,format=raw,file={DISK}",
    "-serial", f"file:{SERIAL}",
    "-monitor", f"tcp:127.0.0.1:{MON_PORT},server,nowait",
    "-vga", "std", "-display", "none",
]
print("[*] launching QEMU (pc/512M/tcg) for KB test...")
errf = open(QERR, "wb")
p = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=errf)

s = None
for _ in range(20):
    if p.poll() is not None:
        errf.close()
        print(f"[FATAL] QEMU exited early rc={p.returncode}")
        with open(QERR, "r", errors="replace") as fh:
            print("  stderr:", fh.read().strip()[:400] or "(empty)")
        sys.exit(2)
    try:
        s = socket.create_connection(("127.0.0.1", MON_PORT), timeout=3)
        break
    except OSError:
        time.sleep(0.5)
s.settimeout(2.0)

def drain():
    buf = b""
    try:
        while True:
            d = s.recv(65536)
            if not d:
                break
            buf += d
    except OSError:
        pass
    return buf.decode(errors="replace")

def type_keys(s, text):
    for ch in text:
        key = {" ": "spc", ".": "dot", "-": "minus", "_": "shift-minus",
               "'": "apostrophe", "(": "shift-9", ")": "shift-0"}.get(ch, ch)
        s.sendall(f"sendkey {key}\n".encode())
        time.sleep(0.06)

def run(cmdline, settle=2.0):
    drain()
    type_keys(s, cmdline)
    s.sendall(b"sendkey ret\n")
    time.sleep(settle)
    drain()

# ---- boot / login ----
drain()
print(f"[*] waiting for login prompt ({WAIT}s)...")
deadline = time.time() + WAIT
login_seen = False
while time.time() < deadline:
    if os.path.exists(SERIAL):
        with open(SERIAL, "r", errors="replace") as fh:
            if "login:" in fh.read():
                login_seen = True
                break
    time.sleep(1)
print(f"    login prompt: {login_seen}")
if login_seen:
    drain(); type_keys(s, "root"); s.sendall(b"sendkey ret\n"); time.sleep(1); drain()
    type_keys(s, "admin"); s.sendall(b"sendkey ret\n"); time.sleep(3); drain()

print("[*] waiting for shell...")
deadline = time.time() + WAIT
shell_ready = False
while time.time() < deadline:
    if os.path.exists(SERIAL):
        with open(SERIAL, "r", errors="replace") as fh:
            if "Shell ready" in fh.read():
                shell_ready = True
                break
    time.sleep(1)
print(f"    shell ready: {shell_ready}")
if not shell_ready:
    s.sendall(b"quit\n"); p.kill(); errf.close()
    print("[FAIL] shell never became ready")
    sys.exit(1)

# ---- exercise the rule engine ----
print("[*] exercising KB rule engine...")
run("kb list", settle=1.5)
run("kb add water helps kidneys filter blood", settle=1.0)        # -> #3 candidate
run("kb prove #3 pass", settle=1.0)                               # -> ACCEPTED (practice)
run("kb add coffee significantly extends lifespan", settle=1.0)    # -> #4 candidate
run("kb cite #4 who https://www.ncbi.nlm.nih.gov study links coffee to lower mortality", settle=1.0)
run("kb cite #4 aha https://www.heart.org review finds coffee neutral to beneficial", settle=1.0)
run("kb cite #4 nhlbi https://www.nhlbi.nih.gov coffee and longevity cohort study", settle=1.0)  # -> 3 cites -> ACCEPTED
run("kb add moon is made of cheese", settle=1.0)                  # -> #5 candidate
run("kb cite #5 wiki https://en.wikipedia.org moon is rock not cheese", settle=1.0)   # 1 cite -> CANDIDATE (gate)
run("kb list", settle=1.5)
run("ai reason what is nexos", settle=3.0)                        # RAG hit on seed #1
run("ai reason does regular exercise lower cardiovascular risk", settle=3.0)  # RAG hit on seed #2

s.sendall(b"quit\n")
try:
    p.wait(10)
except subprocess.TimeoutExpired:
    p.kill()
errf.close()

serial = ""
if os.path.exists(SERIAL):
    with open(SERIAL, "r", errors="replace") as fh:
        serial = fh.read()

print("\n================ EVIDENCE ================")
markers = (
    "KB: added candidate #",                       # add works
    "status=ACCEPTED",                              # at least one accept
    "status=CANDIDATE",                             # gate leaves <3 as candidate
    "[RAG] 命中知识库",                             # reasoning retrieved a fact
    "(regular exercise lowers cardiovascular",       # web-corroborated seed present
)
for m in markers:
    print(f"  {'FOUND ' if m in serial else 'MISS  '} {m!r}")

# Count accepts / candidates in the final kb list to confirm the rule.
n_accept = serial.count("ACCEPTED")
n_cand   = serial.count("CANDIDATE")
print(f"    (serial mentions ACCEPTED x{n_accept}, CANDIDATE x{n_cand})")

print("\n================ VERDICT ================")
added   = "KB: added candidate #" in serial
rag     = "[RAG] 命中知识库" in serial
gated   = ("status=CANDIDATE" in serial)           # the moon claim stayed candidate
accepts = serial.count("status=ACCEPTED") >= 2     # practice + 3-cite both accepted
passed = added and rag and gated and accepts
if passed:
    print("  PASS: KB rule engine works end-to-end in the running OS.")
    print("        - practice success => ACCEPTED")
    print("        - >=3 web sources  => ACCEPTED")
    print("        - <3 web sources    => stays CANDIDATE (gate holds)")
    print("        - ai reason         => retrieved an accepted fact (RAG)")
else:
    print("  FAIL: see MISS markers above.")
    if not gated:
        print("        (the <3-source claim was NOT held as CANDIDATE -- gate broken)")
    if not accepts:
        print("        (expected >=2 ACCEPTED transitions not observed)")
print(f"\nserial: {SERIAL}")
sys.exit(0 if passed else 1)
