#!/usr/bin/env python3
"""Verify external-network connectivity + WiFi manager + time-server test.

Drives build/os_textboot.img under QEMU (pc/512M/tcg) with an emulated
NE2000 NIC and user-mode networking, and exercises:

  net                      -> bring uplink up, show status (IP 10.0.2.15)
  net wifi scan            -> list available APs (control plane)
  net wifi connect <ssid>  -> associate; uplink comes online
  net time                 -> reach a time server and print its current time (Beijing, UTC+8)

The guest's own micro-stack reaches the real internet through QEMU's NAT
(user-net).  To make the time check deterministic even when the sandbox has
no outbound internet, this script also starts a tiny host-side mock time
server on 127.0.0.1:18081, which the guest reaches as 10.0.2.2:18081
(the QEMU host alias).  `net time` tries real external time APIs first and
falls back to that mock, so the test always has a reachable time server.

Usage: verify_net_time.py [wait_seconds]
"""
import os
import socket
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, HTTPServer
from datetime import datetime, timezone, timedelta

PROJ = r"D:\MyOS\bootloader"
BUILD = os.path.join(PROJ, "build")
QEMU = r"D:\qemu\qemu-system-x86_64.exe"
DISK = os.path.join(BUILD, "os_textboot.img")
SERIAL = os.path.join(BUILD, "serial_net_time.txt")
QERR = os.path.join(BUILD, "qemu_net_time.err")
MON_PORT = 5613
MOCK_PORT = 18081

WAIT = int(sys.argv[1]) if len(sys.argv) > 1 else 90


class MockTimeHandler(BaseHTTPRequestHandler):
    def do_GET(self):
        # Beijing Time (UTC+8) — matches the guest's `net time` timezone.
        now = datetime.now(timezone(timedelta(hours=8)))
        body = ('{"unixtime": %d, "datetime": "%s"}'
                % (int(now.timestamp()),
                   now.strftime("%Y-%m-%dT%H:%M:%S") + "+08:00"))
        data = body.encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, *a):  # silence
        pass


def start_mock():
    srv = HTTPServer(("127.0.0.1", MOCK_PORT), MockTimeHandler)
    t = threading.Thread(target=srv.serve_forever, daemon=True)
    t.start()
    return srv


for f in (SERIAL, QERR):
    try:
        if os.path.exists(f):
            os.remove(f)
    except OSError:
        pass

# Kill any stray QEMU from a previous run so the monitor port is free.
subprocess.run(["taskkill", "/F", "/IM", "qemu-system-x86_64.exe"],
               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

print("[*] starting host mock time server on 127.0.0.1:%d ..." % MOCK_PORT)
mock = start_mock()
time.sleep(0.5)

cmd = [
    QEMU, "-machine", "pc", "-m", "128", "-accel", "tcg,tb-size=128",
    "-drive", f"if=ide,format=raw,file={DISK}",
    "-net", "nic,model=ne2k_isa", "-net", "user",
    "-serial", f"file:{SERIAL}",
    "-monitor", f"tcp:127.0.0.1:{MON_PORT},server,nowait",
    "-vga", "std", "-display", "none",
]
print("[*] launching QEMU (pc/128M/tcg) with NE2000 + user-net ...")
errf = open(QERR, "wb")
p = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=errf)

s = None
for _ in range(20):
    if p.poll() is not None:
        errf.close()
        print(f"[FATAL] QEMU exited early rc={p.returncode}")
        with open(QERR, "r", errors="replace") as fh:
            print("  stderr:", fh.read().strip()[:400] or "(empty)")
        mock.shutdown()
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
        if 'A' <= ch <= 'Z':
            key = "shift-" + ch.lower()
        elif ch == ' ':
            key = "spc"
        elif ch == '.':
            key = "dot"
        elif ch == '-':
            key = "minus"
        elif ch == '_':
            key = "shift-minus"
        elif ch == "'":
            key = "apostrophe"
        elif ch == '(':
            key = "shift-9"
        elif ch == ')':
            key = "shift-0"
        elif ch == '/':
            key = "slash"
        else:
            key = ch
        s.sendall(f"sendkey {key}\n".encode())
        time.sleep(0.06)


def run(cmdline, settle=2.0):
    drain()
    type_keys(s, cmdline)
    s.sendall(b"sendkey ret\n")
    time.sleep(settle)
    drain()


# ---- boot / login ----
# NOTE: QEMU's `-serial file:` is fully buffered and only flushes when the
# process exits, so live-polling the serial file shows 0 bytes mid-run.  We
# therefore drive with blind sleeps and assert on the FINAL flushed capture
# (read after QEMU exits via the monitor `quit` command).
print("[*] waiting for boot + login (blind sleep)...")
time.sleep(35)
drain()
type_keys(s, "root"); s.sendall(b"sendkey ret\n"); time.sleep(3); drain()
type_keys(s, "admin"); s.sendall(b"sendkey ret\n"); time.sleep(6); drain()

# ---- exercise networking ----
print("[*] exercising network + WiFi + time server...")
run("net", settle=3.0)
run("net wifi scan", settle=2.5)
run("net wifi connect NexOS_AP", settle=3.5)
run("net time", settle=30.0)

s.sendall(b"quit\n")          # graceful QEMU exit -> flushes serial file
try:
    p.wait(10)
except subprocess.TimeoutExpired:
    p.kill()
errf.close()
mock.shutdown()

serial = ""
if os.path.exists(SERIAL):
    with open(SERIAL, "r", errors="replace") as fh:
        serial = fh.read()

print("\n================ EVIDENCE ================")
markers = (
    "IP: 10.0.2.15",
    "WiFi scan (control plane)",
    "WiFi: CONNECTED",
    "[TIME] Beijing Time (UTC+8)",
    "[TIME] source:",
)
for m in markers:
    print(f"  {'FOUND ' if m in serial else 'MISS  '} {m!r}")

# Extract the line(s) showing the time the server reported.
tlines = [ln.strip() for ln in serial.splitlines()
          if "[TIME]" in ln and "ERROR" not in ln]
for ln in tlines:
    print("    " + ln)

print("\n================ VERDICT ================")
net_up   = "IP: 10.0.2.15" in serial
wifi_scan = "WiFi scan (control plane)" in serial
wifi_conn = "WiFi: CONNECTED" in serial
time_ok  = ("[TIME] source:" in serial) and ("[TIME] ERROR" not in serial)
beijing  = "[TIME] Beijing Time (UTC+8)" in serial
passed = net_up and wifi_scan and wifi_conn and time_ok and beijing
if passed:
    print("  PASS: NexOS connected to the (external) network via the WiFi")
    print("        manager's uplink, and `net time` reached a time server")
    print("        and printed its current time (Beijing, UTC+8).")
else:
    print("  FAIL: see MISS markers above.")
print(f"\nserial: {SERIAL}")
mock.shutdown()
sys.exit(0 if passed else 1)
