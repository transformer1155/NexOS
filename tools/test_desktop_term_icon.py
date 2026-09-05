#!/usr/bin/env python3
"""Does the DESKTOP Terminal icon (index 1) exit the GUI on double-click?
   Single click must NOT exit (just selects)."""
import os, sys, time, socket, subprocess, threading
from test_desktop_ui import (wait_sock, _ser_reader, log_text, wait_for_log,
                              type_line, home, move_to, click)

IMG = "build/os.img"
WORK = "build/os_dti.img"
MPORT = 4501
SPORT = 4502
SER_BUF = bytearray()
import test_desktop_ui as T
# reuse the module's SER_BUF via monkeypatch of globals used by helpers
T.SER_BUF = SER_BUF

# desktop Terminal icon: index 1, box at (22,114) size 84 -> centre (64,156)
TX, TY = 64, 156


def main():
    subprocess.run(["cp", IMG, WORK], check=True)
    errf = open("build/qemu_dti.err", "wb")
    qemu = subprocess.Popen([
        "qemu-system-x86_64", "-drive", f"format=raw,file={WORK}",
        "-m", "128M", "-vga", "std", "-display", "none", "-no-reboot",
        "-monitor", f"tcp:127.0.0.1:{MPORT},server,nowait",
        "-serial", f"tcp:127.0.0.1:{SPORT},server,nowait",
    ], stdout=errf, stderr=errf)
    mon = None
    checks = {}
    try:
        mon = wait_sock(MPORT); mon.settimeout(3.0)
        ser = wait_sock(SPORT)
        threading.Thread(target=_ser_reader, args=(ser,), daemon=True).start()
        try:
            mon.recv(65536)
        except (TimeoutError, socket.timeout):
            pass
        if not wait_for_log("[K5] Hello world written", 90.0):
            print("RESULT: FAIL (boot)"); return 1
        time.sleep(2.0)
        for _ in range(3):
            type_line(mon, "root"); time.sleep(0.6)
            type_line(mon, "admin"); time.sleep(1.5)
            type_line(mon, "echo boot-ok")
            if wait_for_log("boot-ok", 20.0):
                break
            mon.sendall(b"sendkey ret\n"); time.sleep(1.5)
        type_line(mon, "gui")
        if not wait_for_log("[GUI] Entered GUI mode", 30.0):
            print("RESULT: FAIL (no gui)"); return 1
        time.sleep(1.5)

        # clear any prior exit markers
        buf0 = len(SER_BUF)

        # single click -> must NOT exit
        click(mon, TX, TY)
        time.sleep(0.9)
        single_exited = "[GUI] Exited GUI mode" in log_text()[buf0:]
        print("single click exited GUI:", single_exited)
        checks["single_no_exit"] = not single_exited

        # double click -> MUST exit
        click(mon, TX, TY, dbl=True, gap=0.18)
        time.sleep(1.5)
        dbl_exited = "[GUI] Exited GUI mode" in log_text()[buf0:]
        print("double click exited GUI:", dbl_exited)
        checks["dbl_exits"] = dbl_exited

        ok = checks.get("single_no_exit", False) and checks.get("dbl_exits", False)
        print("RESULT:", "PASS" if ok else "FAIL")
        for k, v in checks.items():
            print(f"  {k}: {v}")
        return 0 if ok else 1
    finally:
        try:
            if mon: mon.sendall(b"quit\n")
        except Exception: pass
        qemu.terminate()
        try: qemu.wait(timeout=5)
        except Exception: qemu.kill()


if __name__ == "__main__":
    sys.exit(main())
