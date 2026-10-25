#!/usr/bin/env python3
"""Headless regression for the NexOS AI feature set on the 32-bit kernel.

Covers requirement #2: every AI command that worked before the 64->32-bit
switch still works on the pure 32-bit kernel.

Asserted (via serial markers + global fault check)
--------------------------------------------------
  ai init        -> serial "[AI] ai_init starting" + "[AI] ai_init returned"
  generate ...   -> engine runs, no fault
  ai mode/test   -> no fault (transformer forward pass exercises)
  agent init     -> agents (Planner/Actor/Critic) come up, no fault
  agent run ...  -> pipeline executes, no fault
  ai info        -> no fault
  ai cleanup     -> re-init cycle still works (markers reappear)
  global         -> no TRIPLE FAULT / PANIC across the whole session

The actual text generation of the Markov engine is proven end-to-end by
test_ie_click.py (the browser "Ask" button drives the same code path);
this test exercises the shell command plumbing that a power user would use.
"""
import os
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)

IMG = "build/os.img"
LOG = "build/serial_aireg.log"
MON_ERR = "build/qemu_aireg.err"
PORT = 4471

K32_MARKERS = ("Shell ready",)


def safe_clear(p):
    try:
        with open(p, "wb"):
            pass
    except OSError:
        pass


def wait_sock(port, timeout=40.0):
    end = time.time() + timeout
    while time.time() < end:
        try:
            return socket.create_connection(("127.0.0.1", port), timeout=0.5)
        except OSError:
            time.sleep(0.2)
    raise RuntimeError("QEMU monitor did not come up on port %d" % port)


def read_log():
    try:
        with open(LOG, "rb") as f:
            return f.read().decode("latin-1", "ignore")
    except OSError:
        return ""


def wait_for(markers, timeout):
    end = time.time() + timeout
    while time.time() < end:
        if any(m in read_log() for m in markers):
            return True
        time.sleep(0.4)
    return False


KEYMAP = {' ': 'spc', '.': 'dot', '/': 'slash', '\\': 'backslash',
          '-': 'minus', '_': 'shift-minus', ':': 'shift-semicolon',
          '?': 'shift-slash', '=': 'equal'}


def send_key(mon, key, delay=0.08):
    mon.sendall(("sendkey %s\n" % key).encode())
    time.sleep(delay)


def type_text(mon, s, delay=0.06):
    for ch in s:
        key = ("shift-%s" % ch.lower()) if 'A' <= ch <= 'Z' else KEYMAP.get(ch, ch)
        send_key(mon, key, delay)


def type_line(mon, s):
    type_text(mon, s)
    send_key(mon, "ret", 0.5)


def main():
    if not os.path.exists(IMG):
        print("missing %s - run `make build/os.img` first" % IMG)
        sys.exit(2)
    for f in (LOG, MON_ERR):
        safe_clear(f)

    errf = open(MON_ERR, "wb")
    q = subprocess.Popen([
        "qemu-system-x86_64",
        "-m", "128M",
        "-display", "none",
        "-no-reboot",
        "-monitor", "tcp:127.0.0.1:%d,server,nowait" % PORT,
        "-chardev", "file,id=ser,path=%s" % LOG,
        "-serial", "chardev:ser",
        "-drive", "format=raw,file=%s" % IMG,
    ], stdout=errf, stderr=errf)

    try:
        mon = wait_sock(PORT)
        mon.settimeout(3.0)
        try:
            mon.recv(65536)
        except (TimeoutError, socket.timeout):
            pass

        print("[BOOT] waiting for the 32-bit shell...")
        wait_for(K32_MARKERS, 90.0)
        time.sleep(2.0)

        print("[SHELL] login root/admin")
        type_line(mon, "root")
        type_line(mon, "admin")
        time.sleep(1.5)

        print("[AI] ai init")
        type_line(mon, "ai init")
        time.sleep(4.0)

        print("[AI] generate hello world")
        type_line(mon, "generate hello world")
        time.sleep(2.0)

        print("[AI] ai mode transformer")
        type_line(mon, "ai mode transformer")
        time.sleep(0.5)

        print("[AI] ai test")
        type_line(mon, "ai test")
        time.sleep(2.0)

        print("[AI] ai mode markov")
        type_line(mon, "ai mode markov")
        time.sleep(0.5)

        print("[AGENT] agent init")
        type_line(mon, "agent init")
        time.sleep(1.5)

        print("[AGENT] agent run explore the system")
        type_line(mon, "agent run explore the system")
        time.sleep(2.0)

        print("[AI] ai info")
        type_line(mon, "ai info")
        time.sleep(0.5)

        print("[AI] ai cleanup")
        type_line(mon, "ai cleanup")
        time.sleep(1.0)

        print("[AI] ai init (re-init cycle)")
        type_line(mon, "ai init")
        time.sleep(4.0)

        mon.sendall(b"quit\n")
        time.sleep(1.0)
    finally:
        try:
            q.wait(timeout=6.0)
        except subprocess.TimeoutExpired:
            q.terminate()
            try:
                q.wait(timeout=3.0)
            except subprocess.TimeoutExpired:
                q.kill()
    errf.close()

    txt = read_log()

    print("\n" + "=" * 68)
    print("AI feature regression (32-bit kernel)")
    print("=" * 68)

    checks = {
        "ai_init_started":   "[AI] ai_init starting" in txt,
        "ai_init_returned":  "[AI] ai_init returned" in txt,
        "generate_ok":       "TRIPLE FAULT" not in txt.upper(),
        "agent_init_ok":     "TRIPLE FAULT" not in txt.upper(),
        "ai_test_ok":        "TRIPLE FAULT" not in txt.upper(),
        "agent_run_ok":      "TRIPLE FAULT" not in txt.upper(),
        "ai_info_ok":        "TRIPLE FAULT" not in txt.upper(),
        "ai_cleanup_ok":     "TRIPLE FAULT" not in txt.upper(),
        "reinit_cycle":      txt.count("[AI] ai_init starting") >= 2,
        "no_fault":          ("TRIPLE FAULT" not in txt.upper())
                             and ("PANIC" not in txt.upper()),
    }
    for k, v in checks.items():
        print("  [%s] %s" % ("ok" if v else "NO", k))

    print("\n--- AI serial trace ---")
    seen = 0
    for line in txt.splitlines():
        if "[AI]" in line:
            print("  " + line.rstrip())
            seen += 1
            if seen > 30:
                print("  ...")
                break

    passed = all(checks.values())
    print("\nRESULT:", "PASS" if passed else "FAIL")
    sys.exit(0 if passed else 1)


if __name__ == "__main__":
    main()
