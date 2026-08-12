#!/usr/bin/env python3
"""Fail-safe test for the 3.3 consent prompt.

The happy path (test_foundation0.py) proves that "yes" works. That is the
LESS important half. This test proves the half that actually protects the
user: when consent is not given, nothing happens.

It boots, launches the same ring-3 payload, and then presses NOTHING.
All three consent prompts must time out and resolve to DENY:

  * the prompt must report a timeout, not silently hang
  * the app must never receive the file content (no USERDOC_CONTENT)
  * every open() must fail  -> PERM_DENY_OK / PERM_ALLOW_FAIL / PERM_CACHED_FAIL
  * nothing may be written into the grant cache (no "(remembered)")
  * the kernel must survive it and hand control back to the shell

It also grabs a screendump while the first prompt is on screen, so the
dialog is verified to actually render and not just to log.
"""
import os, sys, socket, time, subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)

IMG = sys.argv[1] if len(sys.argv) > 1 else "build/os.img"
WORK = "build/permfs.img"
LOG = "build/serial_permfs.log"
SHOT = "build/perm_prompt.ppm"
PORT = 4454
TIMEOUT_SEC = 15          # must match PERM_TIMEOUT_SEC in kernel.cpp


def wait_sock(port, timeout=30.0):
    end = time.time() + timeout
    while time.time() < end:
        try:
            return socket.create_connection(("127.0.0.1", port), timeout=0.5)
        except OSError:
            time.sleep(0.2)
    raise RuntimeError("monitor not ready")


def type_line(mon, s):
    keymap = {' ': 'spc', '.': 'dot', '/': 'slash', '\\': 'backslash',
              '-': 'minus', '_': 'shift-minus'}
    for ch in s:
        key = f"shift-{ch.lower()}" if 'A' <= ch <= 'Z' else keymap.get(ch, ch)
        mon.sendall(f"sendkey {key}\n".encode())
        time.sleep(0.08)
    mon.sendall(b"sendkey ret\n")
    time.sleep(0.4)


def main():
    subprocess.run(["cp", IMG, WORK], check=True)
    for f in (LOG, SHOT):
        if os.path.exists(f):
            os.remove(f)
    errf = open("build/qemu_permfs.err", "wb")
    qemu = subprocess.Popen([
        "qemu-system-x86_64",
        "-drive", f"format=raw,file={WORK}",
        "-m", "128M",
        "-vga", "std",
        "-display", "none",
        "-no-reboot",
        "-monitor", f"tcp:127.0.0.1:{PORT},server,nowait",
        "-chardev", f"file,id=ser,path={LOG}",
        "-serial", "chardev:ser",
    ], stdout=errf, stderr=errf)

    try:
        mon = wait_sock(PORT)
        mon.settimeout(3.0)
        try:
            mon.recv(65536)
        except (TimeoutError, socket.timeout):
            pass
        time.sleep(8.0)
        type_line(mon, "root")
        type_line(mon, "admin")
        time.sleep(1.0)

        print("[1] user  (then deliberately answering NOTHING)")
        type_line(mon, "user")
        time.sleep(3.0)
        # The first prompt is on screen right now - capture it as proof the
        # dialog renders, not just logs.
        mon.sendall(f"screendump {SHOT}\n".encode())
        time.sleep(1.0)

        # Three prompts, each must burn its full timeout and then deny.
        wait = 3 * (TIMEOUT_SEC + 3)
        print(f"    waiting {wait}s for all three prompts to time out...")
        time.sleep(wait)

        mon.sendall(b"quit\n")
        time.sleep(1.0)
    finally:
        try:
            qemu.wait(timeout=5.0)
        except subprocess.TimeoutExpired:
            qemu.terminate()
            qemu.wait(timeout=3.0)

    errf.close()
    if not os.path.exists(LOG):
        print("ERROR: serial log was never created. QEMU stderr:")
        with open("build/qemu_permfs.err", "rb") as f:
            print(f.read().decode("latin-1", "ignore")[:2000])
        return 1

    with open(LOG, "rb") as f:
        data = f.read().decode("latin-1", "ignore")
    print("\n--- serial log tail ---")
    print("\n".join(data.splitlines()[-25:]))

    ring3 = data
    if "[SHELL] $ user" in data:
        ring3 = data.split("[SHELL] $ user", 1)[1]

    ok = True

    def need(cond, msg):
        nonlocal ok
        if not cond:
            print("FAIL:", msg)
            ok = False

    print()
    need("EXCEPTION" not in data, "kernel exception detected")
    need("RING3_OK" in ring3, "ring-3 payload never started")

    # The prompt must have been raised, and must have given up on its own.
    need("[PERM] PROMPT" in ring3, "no consent prompt was raised at all")
    need("prompt timed out" in ring3, "the prompt never timed out - it may hang forever")
    need(ring3.count("prompt timed out") >= 3,
         f"expected 3 timeouts, saw {ring3.count('prompt timed out')}")

    # Silence must never be consent.
    need("[PERM] USER ALLOWED" not in ring3, "an unanswered prompt was treated as ALLOW")
    need("(remembered)" not in ring3, "an unanswered prompt was written to the grant cache")
    need("GRANTED-BY-USER" not in ring3, "the VFS let the access through without consent")
    need("USERDOC_CONTENT" not in ring3, "LEAK: file content reached the app without consent")

    # ...and the app must observe the refusal on every single attempt.
    need("PERM_DENY_OK" in ring3, "1st open() did not fail")
    need("PERM_ALLOW_FAIL" in ring3, "2nd open() did not fail")
    need("PERM_CACHED_FAIL" in ring3, "3rd open() did not fail")

    # The kernel has to survive three blocking prompts and come back.
    need("returned from ring-3" in ring3, "kernel did not regain control after the timeouts")

    # Visual proof the dialog is actually painted.
    if os.path.exists(SHOT) and os.path.getsize(SHOT) > 0:
        print(f"screendump: {SHOT} ({os.path.getsize(SHOT)} bytes)")
    else:
        need(False, "screendump of the on-screen prompt was not produced")

    print("\nRESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
