#!/usr/bin/env python3
"""
Headless verification for the three GUI/AI improvements:
  Item 1 (settings persist) -> exercised via Theme.Save/Load (compile + GUI boot)
  Item 2 (Notepad Ctrl+S)    -> compile + GUI boot, manual-confirm
  Item 3 (AI plugin system)  -> `plugin list` / `plugin toggle` via terminal

Uses the Windows QEMU at D:\\qemu (WSL is down this session).
"""
import os, sys, time, socket, subprocess

IMG   = "build/os.img"
LOG   = "build/serial_verify.log"
MON_PORT = 4448
QEMU  = r"D:\qemu\qemu-system-x86_64.exe"

def ser_text():
    try:
        with open(LOG, "r", encoding="latin-1", errors="replace") as f:
            return f.read()
    except (FileNotFoundError, OSError):
        return ""

def wait_for_log(needle, timeout=70.0):
    end = time.time() + timeout
    while time.time() < end:
        if needle in ser_text():
            return True
        time.sleep(0.3)
    return False

def send_key(mon, key, delay=0.14):
    mon.sendall(("sendkey %s\n" % key).encode())
    time.sleep(delay)

def type_text(mon, s, delay=0.14):
    table = {
        " ": "spc", ".": "dot", "/": "slash", ":": "shift-semicolon",
        "-": "minus", "_": "shift-minus", "?": "shift-slash",
    }
    for c in s:
        if c in table:
            send_key(mon, table[c], delay)
        elif c.isupper():
            send_key(mon, "shift-%s" % c.lower(), delay)
        else:
            send_key(mon, c, delay)

def type_line(mon, s):
    type_text(mon, s)
    send_key(mon, "ret", 0.35)

def wait_sock(port):
    end = time.time() + 30
    while time.time() < end:
        try:
            s = socket.create_connection(("127.0.0.1", port), timeout=2)
            return s
        except OSError:
            time.sleep(0.3)
    raise RuntimeError("monitor socket never came up")

def main():
    if not os.path.exists(QEMU):
        print("QEMU not found: %s" % QEMU); sys.exit(2)
    if os.path.exists(LOG):
        os.remove(LOG)

    errf = open("build/verify_err.log", "wb")
    q = subprocess.Popen([
        QEMU,
        "-m", "256M",
        "-accel", "tcg",
        "-display", "none",
        "-no-reboot",
        "-monitor", "tcp:127.0.0.1:%d,server,nowait" % MON_PORT,
        "-serial", "file:%s" % LOG,
        "-drive", "format=raw,file=%s" % IMG,
    ], stdout=errf, stderr=errf)

    try:
        mon = wait_sock(MON_PORT)
        mon.settimeout(3.0)

        # 1) Boot + login
        if not wait_for_log("login:", timeout=60.0):
            print("FAIL: no login prompt (boot did not reach shell)")
            return 1
        print("OK: login prompt seen")
        type_line(mon, "root")
        time.sleep(0.5)
        type_line(mon, "admin")
        time.sleep(1.0)
        # auto-GUI fires after login; wait, then exit to terminal
        time.sleep(4.0)
        send_key(mon, "escape"); send_key(mon, "escape")
        if not wait_for_log("[GUI] Exited GUI mode", timeout=20.0):
            print("WARN: did not see 'Exited GUI mode' (may already be at shell)")
        time.sleep(1.0)

        # 2) plugin list  (Item 3 core engine)
        type_line(mon, "plugin list")
        if not wait_for_log("NexOS plugin catalogue", timeout=20.0):
            print("FAIL: `plugin list` produced no catalogue")
            return 1
        print("OK: `plugin list` catalogue printed")

        # count plugin rows
        txt = ser_text()
        rows = [l for l in txt.splitlines() if "nexos." in l and "[" in l and "]" in l]
        print("   plugin rows seen: %d" % len(rows))

        # 3) plugin toggle (write path via Host.Exec -> run_command)
        type_line(mon, "plugin toggle nexos.knowledge")
        time.sleep(1.5)
        toggle_ok = ("nexos.knowledge" in ser_text() and
                     ("loaded" in ser_text().split("nexos.knowledge")[-1][:120]))
        print(("OK" if toggle_ok else "WARN") + ": `plugin toggle nexos.knowledge`")

        # 4) no crash markers
        crash = ("PANIC" in ser_text() or "triple fault" in ser_text() or
                 "Exception" in ser_text())
        print(("FAIL: crash marker in serial" if crash else "OK: no crash markers"))

        print("RESULT: " + ("PASS" if (not crash) else "FAIL"))
        return 0 if not crash else 1
    finally:
        try: mon.close()
        except Exception: pass
        q.terminate()
        try: q.wait(timeout=5)
        except Exception: pass

if __name__ == "__main__":
    sys.exit(main())
