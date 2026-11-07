#!/usr/bin/env python3
"""Boot NexOS in QEMU and exercise the ring-3 probe / dos commands.

The kernel reads the emulated PS/2 keyboard, so keystrokes must be injected
through the QEMU monitor (`sendkey`), not via stdin on the serial line.
That is exactly what the project's own tools/test_foundation0.py does; this
script follows the same approach but drives the probe/dos commands.
"""

import os, socket, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
NEXROOT = os.path.dirname(HERE)
IMG = os.path.join(NEXROOT, "build", "os_v2.img")
WORK = os.path.join(HERE, "out", "os_probe.img")
LOG = os.path.join(HERE, "out", "probe_serial.log")
PORT = 45471


def wait_sock(port, timeout=20.0):
    end = time.time() + timeout
    while time.time() < end:
        try:
            s = socket.create_connection(("127.0.0.1", port), 1.0)
            return s
        except OSError:
            time.sleep(0.2)
    raise RuntimeError("monitor socket never appeared")


def send_key(mon, key):
    """sendkey, tolerating a dead monitor (the guest may have crashed)."""
    try:
        mon.sendall(f"sendkey {key}\n".encode())
    except (BrokenPipeError, OSError):
        raise MonitorGone()
    time.sleep(0.28)


class MonitorGone(Exception):
    """The guest died: the monitor socket closed mid-sequence."""


def type_line(mon, text):
    """Type a line and press Enter, one key at a time."""
    for ch in text:
        if ch == " ":
            send_key(mon, "spc")
        else:
            send_key(mon, ch)
    send_key(mon, "ret")


def wait_for(mon, needle, timeout=25.0):
    """Poll the serial log until `needle` appears (or time out)."""
    end = time.time() + timeout
    while time.time() < end:
        if os.path.exists(LOG):
            with open(LOG, "rb") as f:
                if needle.encode() in f.read():
                    return True
        time.sleep(0.3)
    return False


def main():
    os.makedirs(os.path.dirname(WORK), exist_ok=True)
    subprocess.run(["cp", IMG, WORK], check=True)
    if os.path.exists(LOG):
        os.remove(LOG)
    errf = open(os.path.join(HERE, "out", "probe_qemu.err"), "wb")

    qemu = subprocess.Popen([
        "qemu-system-x86_64",
        "-drive", f"format=raw,file={WORK}",
        "-m", os.environ.get("PROBE_RAM", "128M"),
        "-vga", "none",          # keep the text shell (no GUI stealing keys)
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

        # Poll for the prompt (log flush makes it appear within a few
        # seconds), but fall back to a fixed wait if it never shows up.
        if wait_for(mon, "login:", timeout=float(os.environ.get("PROBE_BOOT_WAIT", "90"))):
            print("[host] saw login prompt")
        else:
            print("[host] login prompt not seen; typing anyway")
        time.sleep(2.5)
        type_line(mon, "root")
        time.sleep(5.0)
        type_line(mon, "admin")
        time.sleep(5.0)

        # --- the actual bisect sequence ---
        # Each command may kill the guest; catch that and stop cleanly so the
        # partial transcript is still reported (the point is to see how far
        # each attempt got, not to finish the list).
        steps = os.environ.get(
            "PROBE_STEPS",
            "probe minprobe|probe minprobe tiny|probe userdemo"
        ).split("|")
        for cmd in steps:
            cmd = cmd.strip()
            if not cmd:
                continue
            print(f"[host] sending: {cmd}")
            try:
                type_line(mon, cmd)
            except MonitorGone:
                print(f"[host] GUEST DIED while sending: {cmd}")
                break
            time.sleep(6.0)
    finally:
        try:
            mon.close()
        except Exception:
            pass
        qemu.terminate()
        try:
            qemu.wait(timeout=10)
        except subprocess.TimeoutExpired:
            qemu.kill()
        errf.close()

    print("=== serial transcript (probe-related) ===")
    if os.path.exists(LOG):
        with open(LOG, "rb") as f:
            data = f.read().decode("utf-8", "replace")
        for line in data.splitlines():
            if any(k in line for k in
                   ("probe", "MINPROBE", "RING3", "userdemo", "user]",
                    "EXCEPTION", "EIP=", "HALT")):
                print(line)

    print("\n=== checks ===")
    rc = 0
    with open(LOG, "rb") as f:
        blob = f.read()
    for pat in (b"MINPROBE_OK", b"RETURNED", b"returned from ring-3"):
        if pat in blob:
            print(f"ok  : found {pat.decode()}")
        else:
            print(f"MISS: {pat.decode()}")
            rc = 1
    print(f"\n==> {'PASS' if rc == 0 else 'FAIL'}")
    return rc


if __name__ == "__main__":
    sys.exit(main())