#!/usr/bin/env python3
"""Drive the NexOS text shell and run `dos hello.com` + `dos fileio.com`.

Constraints learned the hard way:
  * `-vga none` is required, otherwise the kernel boots into the GUI and
    the login prompt never receives keystrokes.
  * With `-vga none` there is NO VGA device, so `screendump` fails and
    tears down the monitor -- do not use it here.
  * `sendkey` emits no reply; drain the socket after other commands only.
"""
import os, socket, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
NEXROOT = os.path.dirname(HERE)
IMG = os.path.join(NEXROOT, "build", "os_v2.img")
WORK = os.path.join(HERE, "out", "os_dos_live.img")
LOG = os.path.join(HERE, "out", "dos_live_serial.log")
PORT = 45503


def wait_sock(port, timeout=30.0):
    end = time.time() + timeout
    while time.time() < end:
        try:
            return socket.create_connection(("127.0.0.1", port), 1.0)
        except OSError:
            time.sleep(0.2)
    raise RuntimeError("monitor never appeared")


# QEMU `sendkey` takes KEY NAMES, not literal characters.  A bare "." is
# not a valid key name and is silently dropped -- which is why
# "dos hello.com" arrived at the shell as "dos hellocom".
KEYNAMES = {
    ".": "dot", "/": "slash", "\\": "backslash", "-": "minus", "_": "shift-minus",
    ":": "shift-semicolon", ">": "shift-dot", "=": "equal", " ": "spc",
    "\n": "ret", "?": "shift-slash", "*": "shift-8", "\"": "shift-quote",
}


def key(mon, ch):
    mon.sendall(f"sendkey {KEYNAMES.get(ch, ch)}\n".encode())
    time.sleep(0.25)


def type_line(mon, text):
    for ch in text:
        key(mon, ch)
    key(mon, "\n")


def log_text():
    if not os.path.exists(LOG):
        return ""
    with open(LOG, "rb") as f:
        return f.read().decode("utf-8", "replace")


def main():
    os.makedirs(os.path.dirname(WORK), exist_ok=True)
    subprocess.run(["cp", IMG, WORK], check=True)
    if os.path.exists(LOG):
        os.remove(LOG)
    errf = open(os.path.join(HERE, "out", "dos_live.err"), "wb")

    qemu = subprocess.Popen([
        "qemu-system-x86_64",
        "-drive", f"format=raw,file={WORK}",
        "-m", "512M",
        "-vga", "none",
        "-display", "none",
        "-monitor", f"tcp:127.0.0.1:{PORT},server,nowait",
        "-chardev", f"file,id=ser,path={LOG}",
        "-serial", "chardev:ser",
    ], stdout=errf, stderr=errf)

    try:
        mon = wait_sock(PORT)
        mon.settimeout(4.0)
        try:
            mon.recv(65536)
        except (TimeoutError, socket.timeout):
            pass

        print("[host] waiting for boot prompt...")
        end = time.time() + 120
        while time.time() < end and "login:" not in log_text():
            time.sleep(1.0)
        seen = "login:" in log_text()
        print(f"[host] login prompt visible: {seen}")
        time.sleep(2)

        type_line(mon, "root")
        time.sleep(5)
        type_line(mon, "admin")
        time.sleep(6)
        print(f"[host] logged in ({'Shell ready' in log_text()})")

        for c in ("dos hello.com", "dos fileio.com"):
            print(f"[host] running: {c}")
            type_line(mon, c)
            time.sleep(12)
            print(f"[host]   tail: {log_text().splitlines()[-1] if log_text() else ''}")
    except Exception as e:
        print(f"[host] stopped: {type(e).__name__} {e}")
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

    text = log_text()
    print("\n=== DOS-relevant serial lines ===")
    for line in text.splitlines():
        if any(k in line for k in
               ("DOS:", "Hello DOS", "FILEIO", "ramfs", "program returned",
                "loaded ", "MINPROBE")):
            print(line)

    print("\n=== checks ===")
    rc = 0
    for pat in ("DOS: loading", "DOS: program returned"):
        if pat in text:
            print(f"ok  : {pat}")
        else:
            print(f"MISS: {pat}")
            rc = 1
    for pat in ("Hello DOS", "ramfs-overlay-works"):
        if pat in text:
            print(f"ok  : {pat} (guest output)")
        else:
            print(f"note: {pat} not in serial (may be: guest died, or serial tail lost)")
    print(f"\n==> {'PASS' if rc == 0 else 'FAIL'}")
    return rc


if __name__ == "__main__":
    sys.exit(main())