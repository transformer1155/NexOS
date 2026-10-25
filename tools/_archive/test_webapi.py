#!/usr/bin/env python3
"""Headless proof of the webapi password gate (requirement #4 security).

The browser start-page agent interface (`webapi`) is gated by a password
that only the AI assistant knows: it is XOR-folded into the image and never
printed, so a `strings` pass yields nothing.  This test reconstructs the
password from the source exactly the way the kernel does, then proves:

  - a locked session denies every verb          "[webapi] denied: locked"
  - a wrong password fails                       "[webapi] auth failed"
  - the correct password unlocks                 "[webapi] unlocked"
  - an unlocked verb reaches the browser finder
    even with no browser open (graceful)         "[webapi] no IEFrame window"
  - `webapi lock` re-locks the session           denied again
  - three wrong tries wedge the interface so the
    correct password no longer unlocks           unlock count stays put

No browser window is required for the gate itself.  Verb execution against a
live browser (ping / ask / nav / click) is driven end-to-end by
test_ie_click.py, whose "Ask" button calls the same WM_NexOS_API path that
`webapi ask` posts into iexplore.exe.
"""
import os
import re
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)

IMG = "build/os.img"
LOG = "build/serial_webapi.log"
MON_ERR = "build/qemu_webapi.err"
PORT = 4472

K32_MARKERS = ("Shell ready",)


def decode_pw():
    """Rebuild the 24-byte password from the XOR-folded image constant.

    Mirrors webapi_pw_ok(): pw[i] = WEBAPI_PW[i] ^ (0x5A + i*7).  Reading it
    from source keeps the test honest if the password is ever rotated."""
    src = open("kernel.cpp", "rb").read().decode("latin-1", "ignore")
    m = re.search(r"WEBAPI_PW\[24\]\s*=\s*\{([^}]*)\}", src)
    if not m:
        raise RuntimeError("could not find WEBAPI_PW in kernel.cpp")
    nums = [int(x, 0) for x in re.findall(r"0x[0-9A-Fa-f]+", m.group(1))]
    nums = nums[:24]
    raw = bytes([(nums[i] ^ (0x5A + i * 7)) & 0xFF for i in range(24)])
    return raw.decode("latin-1")


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


# Characters that need a shifted key on a US layout.
SPECIAL = {
    ' ': 'spc', '.': 'dot', '/': 'slash', '\\': 'backslash', '-': 'minus',
    '_': 'shift-minus', ':': 'shift-semicolon', '?': 'shift-slash',
    '=': 'equal', '$': 'shift-4', '#': 'shift-3', '&': 'shift-7',
    '!': 'shift-1',
}


def key_for(ch):
    if 'a' <= ch <= 'z':
        return ch
    if 'A' <= ch <= 'Z':
        return "shift-" + ch.lower()
    if '0' <= ch <= '9':
        return ch
    return SPECIAL.get(ch, ch)


def send_key(mon, key, delay=0.08):
    mon.sendall(("sendkey %s\n" % key).encode())
    time.sleep(delay)


def type_text(mon, s, delay=0.06):
    for ch in s:
        send_key(mon, key_for(ch), delay)


def type_line(mon, s):
    type_text(mon, s)
    send_key(mon, "ret", 0.5)


def main():
    if not os.path.exists(IMG):
        print("missing %s - run `make build/os.img` first" % IMG)
        sys.exit(2)
    pw = decode_pw()
    if len(pw) != 24:
        print("decoded password length %d (expected 24)" % len(pw))
        sys.exit(2)
    print("[PW] decoded %d-byte webapi password from source" % len(pw))
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

        # 1. locked session denies verbs
        print("[WEBAPI] status (should be denied: locked)")
        type_line(mon, "webapi status")
        time.sleep(0.6)

        # 2. correct password unlocks
        print("[WEBAPI] auth <correct>")
        type_line(mon, "webapi auth " + pw)
        time.sleep(0.6)

        # 3. unlocked verb reaches the browser finder (no browser -> graceful)
        print("[WEBAPI] status (unlocked, no browser)")
        type_line(mon, "webapi status")
        time.sleep(0.6)

        # 4. lock again
        print("[WEBAPI] lock")
        type_line(mon, "webapi lock")
        time.sleep(0.6)

        # 5. three wrong passwords -> wedge
        print("[WEBAPI] auth wrong x3")
        type_line(mon, "webapi auth WRONGPASSWORD01")
        time.sleep(0.5)
        type_line(mon, "webapi auth WRONGPASSWORD02")
        time.sleep(0.5)
        type_line(mon, "webapi auth WRONGPASSWORD03")
        time.sleep(0.5)

        # capture unlock count BEFORE the final (should-fail) correct attempt
        unlocked_before = read_log().count("[webapi] unlocked")

        # 6. correct password now rejected (locked out)
        print("[WEBAPI] auth <correct> (should be locked out)")
        type_line(mon, "webapi auth " + pw)
        time.sleep(0.6)

        # 7. still denied
        print("[WEBAPI] status (should still be denied)")
        type_line(mon, "webapi status")
        time.sleep(0.6)

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
    unlocked_after = txt.count("[webapi] unlocked")

    print("\n" + "=" * 68)
    print("webapi password gate (32-bit kernel)")
    print("=" * 68)

    checks = {
        "locked_denies":        "[webapi] denied: locked" in txt,
        "correct_unlocks":      "[webapi] unlocked" in txt,
        "unlocked_reaches_find":"[webapi] no IEFrame window" in txt,
        "lock_relocks":         txt.count("[webapi] denied: locked") >= 2,
        "wrong1_fails":         txt.count("[webapi] auth failed") >= 1,
        "wrong3_fails":         txt.count("[webapi] auth failed") >= 3,
        "lockout_blocks_right": unlocked_after == unlocked_before,
        "no_fault":             ("TRIPLE FAULT" not in txt.upper())
                             and ("PANIC" not in txt.upper()),
    }
    for k, v in checks.items():
        print("  [%s] %s" % ("ok" if v else "NO", k))

    print("\n--- webapi serial trace ---")
    seen = 0
    for line in txt.splitlines():
        if "[webapi]" in line:
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
