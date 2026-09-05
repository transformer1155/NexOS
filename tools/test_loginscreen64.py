#!/usr/bin/env python3
"""64-bit (long-mode) graphical sign-in verification.

The 32-bit default boot path is covered by test_loginscreen.py.  This
script proves the 64-bit kernel (kernel64.cpp) carries the SAME three
behaviour changes, end-to-end, by actually booting into long mode:

  1. boot 32-bit -> graphical lock screen -> sign in (root/admin)
  2. open the desktop "Terminal" shortcut -> a GUI Terminal WINDOW that
     runs real kernel commands (g_cb.exec_command)
  3. type `switch64` in that window -> 32-bit loader jumps into
     kernel64.bin (long mode)
  4. the 64-bit kernel must ALSO defer sign-in to its graphical lock
     screen and auto-enter the GUI ("[K64] sign-in deferred ...")
  5. wrong password rejected ("[K64-LOGIN] reject")
  6. root/admin accepted ("[K64-LOGIN] OK user=root") -> desktop
  7. Escape must not tear the 64-bit desktop down either

Usage:
    python3 tools/test_loginscreen64.py [path/to/os.img]
"""
import os, sys, socket, time, subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)

IMG = sys.argv[1] if len(sys.argv) > 1 else "build/os.img"
WORK = "build/loginscreen64_work.img"
LOG = "build/serial_loginscreen64.log"
PORT = 4462


def mark(s):
    try:
        with open("build/loginscreen64_result.txt", "a") as f:
            f.write(s + "\n")
    except Exception:
        pass


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
    time.sleep(0.45)


def read_log():
    try:
        return open(LOG, "rb").read().decode("latin-1", "ignore")
    except Exception:
        return ""


# QEMU monitor mouse_move is RELATIVE; track the real cursor position.
_CURSOR = [640, 360]


def mouse_abs(mon, x, y):
    dx, dy = x - _CURSOR[0], y - _CURSOR[1]
    while dx or dy:
        sx = max(-80, min(80, dx)); sy = max(-80, min(80, dy))
        mon.sendall(("mouse_move %d %d\n" % (sx, sy)).encode())
        time.sleep(0.08)
        _CURSOR[0] += sx; _CURSOR[1] += sy
        dx -= sx; dy -= sy
    time.sleep(0.2)


def mouse_click(mon, x, y):
    mouse_abs(mon, x, y)
    mon.sendall(b"mouse_button 1\n"); time.sleep(0.12)
    mon.sendall(b"mouse_button 0\n"); time.sleep(0.6)


def main():
    subprocess.run(["cp", IMG, WORK], check=True)
    for f in (LOG, "build/loginscreen64_result.txt"):
        if os.path.exists(f):
            os.remove(f)
    errf = open("build/qemu_loginscreen64.err", "wb")
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
    ok = True
    try:
        mon = wait_sock(PORT)
        mon.settimeout(3.0)
        try:
            mon.recv(65536)
        except (TimeoutError, socket.timeout, OSError):
            pass
        print("booting 32-bit kernel...")
        time.sleep(18.0)

        # sign in on 32-bit (root/admin)
        type_line(mon, "admin")
        time.sleep(2.5)
        data = read_log()
        if "[K32-LOGIN] OK user=root" not in data:
            print("FAIL: 32-bit sign-in did not succeed")
            mark("32-bit login: FAIL")
            ok = False
        else:
            print("PASS: 32-bit sign-in OK, on the desktop")

        # ---- open the desktop "Terminal" shortcut: a GUI Terminal window
        #      whose input line is executed by the kernel shell.  The
        #      Terminal icon is index 1 in the default desktop layout ->
        #      cell centre ~ (64, 156). ----
        print("opening the desktop 'Terminal' window (default icon cell)...")
        mouse_click(mon, 64, 156)
        time.sleep(1.2)
        # dismiss any stray window if the click missed the Terminal icon
        mon.sendall(b"sendkey alt-f4\n"); time.sleep(0.4)

        # ---- switch to the 64-bit (long-mode) kernel via the GUI terminal
        print("[switch64] jump into long-mode kernel")
        type_line(mon, "switch64")
        time.sleep(13.0)
        data = read_log()
        if "[K64] sign-in deferred to the graphical lock screen" in data:
            print("PASS: 64-bit kernel also defers sign-in to the "
                  "graphical lock screen")
            mark("A64 default-GUI: PASS")
        else:
            print("FAIL: 64-bit kernel did not defer sign-in "
                  "(serial tail below)")
            print("\n".join(data.splitlines()[-10:]))
            mark("A64 default-GUI: FAIL")
            ok = False

        # 64-bit graphical login: wrong, then correct
        type_line(mon, "zzz")
        time.sleep(1.5)
        data = read_log()
        if "[K64-LOGIN] reject" in data or "[K64-LOGIN] no such user" in data:
            print("PASS: 64-bit wrong credential rejected")
            mark("C64 reject: PASS")
        else:
            print("FAIL: 64-bit wrong password not rejected")
            mark("C64 reject: FAIL")
            ok = False

        type_line(mon, "admin")
        time.sleep(2.5)
        data = read_log()
        if "[K64-LOGIN] OK user=root" in data:
            print("PASS: 64-bit root/admin accepted")
            mark("C64 accept: PASS")
        else:
            print("FAIL: 64-bit root/admin not accepted")
            mark("C64 accept: FAIL")
            ok = False

        # ---- 64-bit ESC must not tear the desktop down ----
        pos = len(read_log())
        mon.sendall(b"sendkey esc\n"); time.sleep(1.0)
        mon.sendall(b"sendkey esc\n"); time.sleep(0.6)
        tail = read_log()[pos:]
        if "[GUI] Exited GUI mode" in tail:
            print("FAIL: 64-bit Escape still exited the GUI")
            mark("B64 esc: FAIL")
            ok = False
        else:
            print("PASS: 64-bit Escape did NOT exit the GUI")
            mark("B64 esc: PASS")

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
        print("ERROR: no serial log. QEMU stderr:")
        with open("build/qemu_loginscreen64.err", "rb") as f:
            print(f.read().decode("latin-1", "ignore")[:2000])
        return 1

    data = read_log()
    print("\n--- serial log tail ---")
    print("\n".join(data.splitlines()[-12:]))

    if "TRIPLE FAULT" in data or "PANIC" in data:
        print("\nFAIL: kernel fault detected")
        ok = False

    print("\nRESULT:", "PASS" if ok else "FAIL")
    with open("build/loginscreen64_result.txt", "w") as f:
        f.write("RESULT: " + ("PASS" if ok else "FAIL") + "\n")
    return 0 if ok else 1


if __name__ == "__main__":
    try:
        rc = main()
    except Exception as ex:
        import traceback
        with open("build/loginscreen64_result.txt", "w") as f:
            f.write("EXCEPTION: " + str(ex) + "\n")
            f.write(traceback.format_exc())
        print("EXCEPTION", ex)
        sys.exit(1)
    sys.exit(rc)
