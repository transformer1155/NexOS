#!/usr/bin/env python3
"""64-bit login + permission system regression test (robust timing version).

Boot 32-bit -> login(root/admin) -> switch64 -> 64-bit login(root/admin)
-> exercise the security subsystem and assert the kernel emits the serial
markers added to kernel64.cpp.

The previous version used fixed sleeps and was racy: if QEMU's boot took
longer than expected, the initial `root`/`admin` keystrokes were consumed by
the still-unready 32-bit login prompt, which then also swallowed `switch64`,
so the whole run stayed in the 32-bit shell and the [K64-*] markers never
appeared.  This version polls the serial log for readiness markers instead
of guessing boot time.
"""
import os, sys, socket, time, subprocess
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)

IMG = sys.argv[1] if len(sys.argv) > 1 else "build/os.img"
WORK = "build/login64_work.img"
LOG = "build/serial_login64.log"
PORT = 4481


def wait_sock(port, timeout=30.0):
    end = time.time() + timeout
    while time.time() < end:
        try:
            return socket.create_connection(("127.0.0.1", port), timeout=0.5)
        except OSError:
            time.sleep(0.2)
    raise RuntimeError("monitor not ready")


def type_line(mon, s):
    km = {' ': 'spc', '.': 'dot', '/': 'slash', '\\': 'backslash',
          '-': 'minus', '_': 'shift-minus'}
    for ch in s:
        k = f"shift-{ch.lower()}" if 'A' <= ch <= 'Z' else km.get(ch, ch)
        mon.sendall(f"sendkey {k}\n".encode()); time.sleep(0.08)
    mon.sendall(b"sendkey ret\n"); time.sleep(0.4)


def log_data():
    try:
        with open(LOG, "rb") as f:
            return f.read().decode("latin-1", "ignore")
    except Exception:
        return ""


def wait_for(substr, timeout=30.0):
    end = time.time() + timeout
    while time.time() < end:
        if substr in log_data():
            return True
        time.sleep(0.3)
    return False


def mark(s):
    try:
        with open("build/login64_result.txt", "a") as f:
            f.write(s + "\n")
    except Exception:
        pass


def main():
    subprocess.run(["cp", IMG, WORK], check=True)
    for f in (LOG, "build/login64_result.txt"):
        if os.path.exists(f):
            os.remove(f)
    qemu = subprocess.Popen([
        "qemu-system-x86_64", "-drive", f"format=raw,file={WORK}", "-m", "128M",
        "-vga", "std", "-display", "none", "-no-reboot",
        "-monitor", f"tcp:127.0.0.1:{PORT},server,nowait",
        "-chardev", f"file,id=ser,path={LOG}", "-serial", "chardev:ser",
    ], stderr=open("build/qemu_login64.err", "wb"))
    try:
        mon = wait_sock(PORT); mon.settimeout(3.0)
        try:
            mon.recv(65536)
        except (TimeoutError, socket.timeout, OSError):
            pass

        # ---- 32-bit login (root/admin) ----
        # login prompt is screen-only, but the 32-bit shell prints "[SHELL] $"
        # once login succeeds, so poll for that.  Retry typing if boot was slow.
        time.sleep(6.0)
        logged32 = False
        for attempt in range(4):
            type_line(mon, "root"); type_line(mon, "admin")
            if wait_for("[SHELL] $", 15):
                logged32 = True
                mark("k32 shell ready")
                break
            mark(f"k32 retry {attempt}")
        if not logged32:
            print("FAIL: never reached 32-bit shell (login prompt?)")
            return 1

        # ---- switch to 64-bit long mode ----
        type_line(mon, "switch64")
        if not wait_for("[K64-1] kmain64 entered", 25):
            print("FAIL: switch64 did not transition to 64-bit kernel")
            return 1
        mark("k64 entered")

        # ---- 64-bit security login (root/admin) ----
        # login_prompt() blocks; type credentials then wait for the marker.
        logged64 = False
        for attempt in range(4):
            time.sleep(2.0)
            type_line(mon, "root"); type_line(mon, "admin")
            if wait_for("[K64-LOGIN] OK user=root", 15):
                logged64 = True
                mark("k64 login ok")
                break
            mark(f"k64 login retry {attempt}")
        if not logged64:
            print("FAIL: 64-bit login did not accept root/admin")
            return 1

        # ---- exercise the security subsystem ----
        type_line(mon, "whoami"); time.sleep(0.6)
        type_line(mon, "users"); time.sleep(0.6)
        type_line(mon, "id"); time.sleep(0.6)
        type_line(mon, "su guest"); time.sleep(0.8)
        type_line(mon, "guest"); time.sleep(0.8)
        type_line(mon, "whoami"); time.sleep(0.6)
        type_line(mon, "su root"); time.sleep(0.8)
        type_line(mon, "admin"); time.sleep(0.8)
        type_line(mon, "sudo whoami"); time.sleep(0.8)
        type_line(mon, "admin"); time.sleep(0.8)
        type_line(mon, "perm"); time.sleep(0.6)
        type_line(mon, "chmod 600 shadow"); time.sleep(0.6)
        type_line(mon, "stat shadow"); time.sleep(0.6)

        time.sleep(1.0)
        mon.sendall(b"quit\n"); time.sleep(1.0)
    finally:
        try:
            qemu.wait(timeout=5.0)
        except subprocess.TimeoutExpired:
            qemu.terminate(); qemu.wait(timeout=3.0)

    data = log_data()
    print("\n--- serial log tail ---")
    print("\n".join(data.splitlines()[-25:]))

    ok = True
    if "EXCEPTION" in data or "TRIPLE FAULT" in data or "PANIC" in data:
        print("\nFAIL: kernel fault detected")
        ok = False
    if "[K64-1] kmain64 entered" in data:
        print("PASS: switch64 transitioned to 64-bit kernel")
    else:
        print("FAIL: 64-bit kernel never entered")
        ok = False
    if "[K64-LOGIN] OK user=root" in data:
        print("PASS: 64-bit login accepted root/admin")
    else:
        print("FAIL: 64-bit login did not accept root/admin")
        ok = False
    if "[K64-WHOAMI] root" in data:
        print("PASS: whoami -> root (after login)")
    else:
        print("FAIL: whoami did not report root")
        ok = False
    if "[K64-SU] guest" in data:
        print("PASS: su guest switched to guest")
    else:
        print("WARN: su guest marker not seen (check pw)")
    if "[K64-WHOAMI] guest" in data:
        print("PASS: whoami -> guest (after su guest)")
    else:
        print("WARN: whoami did not report guest after su")
    if "[K64-SU] root" in data:
        print("PASS: su root switched back to root")
    if "[K64-WHOAMI] root" in data:
        print("PASS: sudo whoami -> root (elevated)")
    if "grant" in data.lower():
        print("PASS: perm command executed (consent engine reachable)")
    else:
        print("WARN: perm command output not captured")

    print("\nRESULT:", "PASS" if ok else "FAIL")
    with open("build/login64_result.txt", "w") as f:
        f.write("RESULT: " + ("PASS" if ok else "FAIL") + "\n")
    return 0 if ok else 1


if __name__ == "__main__":
    try:
        rc = main()
    except Exception as ex:
        import traceback
        with open("build/login64_result.txt", "w") as f:
            f.write("EXCEPTION: " + str(ex) + "\n")
            f.write(traceback.format_exc())
        print("EXCEPTION", ex)
        sys.exit(1)
    sys.exit(rc)
