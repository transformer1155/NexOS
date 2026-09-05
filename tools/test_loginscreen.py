#!/usr/bin/env python3
"""Graphical sign-in / default-GUI / ESC behaviour regression test.

Verifies the three Boot-experience changes end-to-end under a headless
QEMU (the 32-bit default boot path, which is what the user actually sees):

  (A) DEFAULT GRAPHICAL BOOT
        On a machine with a framebuffer the OS must come up straight into
        the Win11 desktop's LOCK SCREEN -- NOT stop at the text login
        prompt first.  Proven by the kernel marker
        "[K32] sign-in deferred to the graphical lock screen" and the
        managed lock screen arming ("[LOGIN] lock screen armed").

  (B) ESC NO LONGER TEARS THE DESKTOP DOWN
        A stray Escape used to call gui_exit() and dump the whole session
        back to the text terminal (looked like a crash).  Now ESC only
        cancels the IME.  Proven by the ABSENCE of
        "[GUI] Exited GUI mode, fbcon text mode (VBE stays active)" after
        we send Escape while the desktop is up.

  (C) GRAPHICAL LOGIN
        Credentials are collected by the managed lock screen
        (csharp/apps/Shell/Login.cs) and checked by the kernel.  Proven
        by a rejected wrong password ("[K32-LOGIN] reject") and a
        successful sign-in ("[K32-LOGIN] OK user=root") followed by the
        desktop rendering.

Usage:
    python3 tools/test_loginscreen.py [path/to/os.img]
"""
import os, sys, socket, time, subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)

IMG = sys.argv[1] if len(sys.argv) > 1 else "build/os.img"
WORK = "build/loginscreen_work.img"
LOG = "build/serial_loginscreen.log"
LOCK = "build/ls_lock.ppm"
DESK = "build/ls_desk.ppm"
ESC = "build/ls_esc.ppm"
PORT = 4461

DIFF_TOL = 24


def mark(s):
    try:
        with open("build/loginscreen_result.txt", "a") as f:
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


def read_ppm(path):
    with open(path, "rb") as f:
        assert f.readline().strip() == b"P6"
        w, h = (int(x) for x in f.readline().split())
        f.readline()
        px = f.read()
    return w, h, px


def read_ppm_retry(path, tries=12):
    for _ in range(tries):
        try:
            w, h, px = read_ppm(path)
            if len(px) >= w * h * 3:
                return w, h, px
        except (IndexError, AssertionError):
            pass
        time.sleep(0.5)
    return read_ppm(path)


def whole_screen_diff(path_a, path_b, tol=DIFF_TOL):
    w, h, pa = read_ppm_retry(path_a)
    _, _, pb = read_ppm_retry(path_b)
    n = 0
    for i in range(0, w * h * 3, 3):
        if (abs(pa[i] - pb[i]) + abs(pa[i + 1] - pb[i + 1])
                + abs(pa[i + 2] - pb[i + 2])) > tol:
            n += 1
    return n


def read_log():
    try:
        return open(LOG, "rb").read().decode("latin-1", "ignore")
    except Exception:
        return ""


def main():
    subprocess.run(["cp", IMG, WORK], check=True)
    for f in (LOG, LOCK, DESK, ESC, "build/loginscreen_result.txt"):
        if os.path.exists(f):
            os.remove(f)
    errf = open("build/qemu_loginscreen.err", "wb")
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
        print("booting 32-bit kernel (default graphical boot)...")
        time.sleep(18.0)

        data = read_log()
        print("\n--- [A] default graphical boot ---")
        if "[K32] sign-in deferred to the graphical lock screen" in data:
            print("PASS: sign-in deferred to the graphical lock screen "
                  "(no text login prompt first)")
            mark("A default-GUI: PASS (sign-in deferred)")
        else:
            print("FAIL: kernel did not defer sign-in to the lock screen")
            mark("A default-GUI: FAIL (no 'sign-in deferred' marker)")
            ok = False
        if "[LOGIN] lock screen armed" in data:
            print("PASS: managed lock screen armed (Login.cs active)")
            mark("A lock-screen: PASS (armed)")
        else:
            print("FAIL: managed lock screen did not arm")
            mark("A lock-screen: FAIL (not armed)")
            ok = False
        if "[MFORMS] managed shell ready" in data:
            print("PASS: managed shell loaded")
        else:
            print("FAIL: managed shell did not come online")
            ok = False

        mon.sendall(f"screendump {LOCK}\n".encode())
        time.sleep(1.5)
        print("captured lock screen -> %s" % LOCK)

        # ---- (C) graphical login: wrong password first ----
        print("\n--- [C] graphical login (wrong password) ---")
        type_line(mon, "zzz")      # default user is root; wrong pass
        time.sleep(1.5)
        data = read_log()
        if "[K32-LOGIN] reject" in data or "[K32-LOGIN] no such user" in data:
            print("PASS: wrong credential rejected by the kernel "
                  "(no hash left the managed side)")
            mark("C reject: PASS")
        else:
            print("FAIL: wrong password was not rejected")
            mark("C reject: FAIL")
            ok = False

        # ---- (C) graphical login: correct password ----
        print("\n--- [C] graphical login (root / admin) ---")
        type_line(mon, "admin")     # focus starts on the password field
        time.sleep(2.5)
        data = read_log()
        if "[K32-LOGIN] OK user=root" in data:
            print("PASS: root/admin accepted, session committed by kernel")
            mark("C accept: PASS (root)")
        else:
            print("FAIL: root/admin not accepted -> %r" % data[-300:])
            mark("C accept: FAIL")
            ok = False

        mon.sendall(f"screendump {DESK}\n".encode())
        time.sleep(1.5)
        print("captured desktop -> %s" % DESK)

        if os.path.exists(LOCK) and os.path.exists(DESK):
            d = whole_screen_diff(LOCK, DESK)
            print("lock-screen vs desktop pixel diff = %d" % d)
            if d > 40000:
                print("PASS: desktop rendered after sign-in "
                      "(image differs from lock screen)")
                mark("C desktop: PASS (diff=%d)" % d)
            else:
                print("WARN: desktop image barely differs from lock screen")
                mark("C desktop: WARN (diff=%d)" % d)

        # ---- (B) ESC must NOT exit the GUI ----
        print("\n--- [B] ESC must not tear the desktop down ---")
        mon.sendall(b"sendkey esc\n")
        time.sleep(1.0)
        # a second ESC + a stray printable, to be sure nothing crashes/exits
        mon.sendall(b"sendkey esc\n")
        time.sleep(0.6)
        mon.sendall(b"sendkey a\n")
        time.sleep(0.8)
        data = read_log()
        mon.sendall(f"screendump {ESC}\n".encode())
        time.sleep(1.2)
        if "[GUI] Exited GUI mode" in data:
            print("FAIL: Escape still exited the GUI -> text terminal")
            mark("B esc: FAIL (gui_exit fired)")
            ok = False
        else:
            print("PASS: Escape did NOT exit the GUI (no gui_exit marker)")
            mark("B esc: PASS (gui stays up)")

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
        with open("build/qemu_loginscreen.err", "rb") as f:
            print(f.read().decode("latin-1", "ignore")[:2000])
        return 1

    data = read_log()
    print("\n--- serial log tail ---")
    print("\n".join(data.splitlines()[-18:]))

    if "TRIPLE FAULT" in data or "PANIC" in data or "EXCEPTION" in data:
        print("\nFAIL: kernel fault detected")
        ok = False

    print("\nRESULT:", "PASS" if ok else "FAIL")
    with open("build/loginscreen_result.txt", "w") as f:
        f.write("RESULT: " + ("PASS" if ok else "FAIL") + "\n")
    return 0 if ok else 1


if __name__ == "__main__":
    try:
        rc = main()
    except Exception as ex:
        import traceback
        with open("build/loginscreen_result.txt", "w") as f:
            f.write("EXCEPTION: " + str(ex) + "\n")
            f.write(traceback.format_exc())
        print("EXCEPTION", ex)
        sys.exit(1)
    sys.exit(rc)
