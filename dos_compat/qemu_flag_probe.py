#!/usr/bin/env python3
"""Ring-3 iret discriminator via QEMU physical-memory inspection.

Hypothesis A: enter_user()'s `iret` never lands in ring 3.
Hypothesis B: the iret DOES land and the hang happens later.

The `spinloop` payload is the discriminator:

        mov byte [USER_BASE+0x800], 0xAB   ; one store, then spin
        jmp $

After `probe spinloop` the guest hangs either way, so we cannot read the
flag from inside the guest.  Instead we ask the QEMU monitor to examine
PHYSICAL memory directly (`xp`), which works even while the guest is stuck:

   * 0xAB at 0x04000800  -> ring-3 store executed -> iret landed (B)
   * 0x00                -> ring-3 never ran      -> iret is the blocker (A)

The page tables are identity-mapped, so VA == PA here.
"""

import os, socket, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
NEXROOT = os.path.dirname(HERE)
IMG = os.path.join(NEXROOT, "build", "os_v2.img")
WORK = os.path.join(HERE, "out", "os_flag.img")
LOG = os.path.join(HERE, "out", "flag_serial.log")
PORT = 45493
FLAG_PA = 0x04000800


class MonitorGone(Exception):
    pass


def wait_sock(port, timeout=25.0):
    end = time.time() + timeout
    while time.time() < end:
        try:
            return socket.create_connection(("127.0.0.1", port), 1.0)
        except OSError:
            time.sleep(0.2)
    raise RuntimeError("monitor socket never appeared")


def send_key(mon, key):
    try:
        mon.sendall(f"sendkey {key}\n".encode())
    except (BrokenPipeError, OSError):
        raise MonitorGone()
    time.sleep(0.28)


def type_line(mon, text):
    for ch in text:
        send_key(mon, "spc" if ch == " " else ch)
    send_key(mon, "ret")


def log_text():
    if not os.path.exists(LOG):
        return ""
    with open(LOG, "rb") as f:
        return f.read().decode("utf-8", "replace")


def wait_for(needle, timeout=60.0):
    end = time.time() + timeout
    while time.time() < end:
        if needle in log_text():
            return True
        time.sleep(0.3)
    return False


def login(mon):
    wait_for("login:", timeout=70.0)
    time.sleep(1.0)
    type_line(mon, "root")
    wait_for("Password:", timeout=25.0)
    time.sleep(1.0)
    type_line(mon, "admin")
    wait_for("Shell ready", timeout=25.0)
    time.sleep(1.5)


def xp_byte(mon):
    """Read the flag byte via the human monitor, tolerating async replies."""
    mon.sendall(f"xp /1xb 0x{FLAG_PA:08x}\n".encode())
    time.sleep(0.6)
    try:
        return mon.recv(65536).decode("utf-8", "replace")
    except (TimeoutError, socket.timeout):
        return ""


def main():
    os.makedirs(os.path.dirname(WORK), exist_ok=True)
    subprocess.run(["cp", IMG, WORK], check=True)
    if os.path.exists(LOG):
        os.remove(LOG)
    errf = open(os.path.join(HERE, "out", "flag_qemu.err"), "wb")

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

    before = after = ""
    try:
        mon = wait_sock(PORT)
        mon.settimeout(3.0)
        try:
            mon.recv(65536)          # banner
        except (TimeoutError, socket.timeout):
            pass

        login(mon)

        # baseline BEFORE running the payload
        before = xp_byte(mon)
        print(f"[host] flag BEFORE probe : {before.strip() or '(no reply)'}")

        print("[host] running: probe spinloop (stores, then spins)")
        try:
            type_line(mon, "probe spinloop")
        except MonitorGone:
            print("[host] monitor dropped while typing")
        wait_for("[probe] loaded", timeout=30.0)
        time.sleep(8.0)          # give the guest time to store + spin

        # read physical memory WHILE the guest is stuck
        after = xp_byte(mon)
        print(f"[host] flag AFTER  probe : {after.strip() or '(no reply)'}")
    except Exception as e:
        print(f"[host] error: {type(e).__name__} {e}")
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

    print("\n=== transcript ===")
    for line in log_text().splitlines():
        if any(k in line for k in ("probe", "Two-stage", "EXCEPTION", "EIP=")):
            print(line)

    print("\n=== verdict ===")
    blob = (before + after)
    if "0xab" in blob:
        print("STORE EXECUTED -> the iret LANDS in ring 3; blocker is downstream")
    elif "0x00" in after:
        print("NO STORE       -> the iret NEVER LANDS; blocker is enter_user")
    else:
        print("INCONCLUSIVE   -> xp gave no usable reply")
        print(f"   before={before!r}")
        print(f"   after ={after!r}")
    return 0


if __name__ == "__main__":
    sys.exit(main())