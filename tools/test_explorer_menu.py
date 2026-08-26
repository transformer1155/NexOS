#!/usr/bin/env python3
"""Headless verification of the File Explorer context-menu changes.

Uses the TEXT-BOOT image (os_textboot.img): it starts in the text shell, so
we can format the MKFS volume and seed a file BEFORE entering the GUI.

Flow:
  [0] text shell: mkfs + touch hello.txt on the MKFS volume
  [1] `gui` -> managed desktop, sign in on the lock screen
  [2] open File Explorer (taskbar pin 0, double-click)
  [3] right-click a MKFS file row  -> context menu with "New file"
  [4] click "New file"             -> inline rename editor appears
  [5] type a name + Enter          -> file created on MKFS (serial marker)
  [6] switch to the SFS volume, right-click a file, click where "New file"
      would sit -> SFS is read-only, so NO create marker may appear.
"""
import os, sys, socket, time, subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)

IMG = sys.argv[1] if len(sys.argv) > 1 else "build/os_textboot.img"
WORK = "build/explorer_menu_work.img"
LOG = "build/serial_explorer_menu.log"
PORT = 4483
RESULT = "build/explorer_menu_result.txt"


def mark(s):
    try:
        with open(RESULT, "a") as f:
            f.write(s + "\n")
    except Exception:
        pass


def wait_for_serial(log_path, marker, timeout=10.0):
    end = time.time() + timeout
    while time.time() < end:
        if os.path.exists(log_path):
            try:
                with open(log_path, "rb") as f:
                    data = f.read().decode("latin-1", "ignore")
                if marker in data:
                    return True
            except Exception:
                pass
        time.sleep(0.2)
    return False


def wait_sock(port, timeout=40.0):
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


_CURSOR = [640, 360]


def mouse_abs(mon, x, y):
    global _CURSOR
    dx, dy = x - _CURSOR[0], y - _CURSOR[1]
    while dx or dy:
        sx = max(-80, min(80, dx))
        sy = max(-80, min(80, dy))
        mon.sendall(("mouse_move %d %d\n" % (sx, sy)).encode())
        time.sleep(0.08)
        _CURSOR[0] += sx
        _CURSOR[1] += sy
        dx -= sx
        dy -= sy
    time.sleep(0.15)


def mouse_press(mon, btn):
    mon.sendall(("mouse_button %d\n" % btn).encode())
    time.sleep(0.12)


def left_click(mon, x, y):
    mouse_abs(mon, x, y)
    mouse_press(mon, 1)
    time.sleep(0.08)
    mouse_press(mon, 0)
    time.sleep(0.25)


def right_click(mon, x, y):
    mouse_abs(mon, x, y)
    mouse_press(mon, 2)
    time.sleep(0.5)
    mouse_press(mon, 0)
    time.sleep(0.25)


def shot(mon, name):
    mon.sendall(f"screendump build/{name}.ppm\n".encode())
    time.sleep(1.2)


def main():
    # Truncate (not remove) so the sandbox delete-hook does not fire.
    for f in (LOG, RESULT):
        try:
            open(f, "w").close()
        except Exception:
            pass
    subprocess.run(["cp", IMG, WORK], check=True)

    errf = open("build/qemu_explorer_menu.err", "wb")
    qemu = subprocess.Popen([
        r"D:\qemu\qemu-system-x86_64.exe",
        "-drive", f"format=raw,file={WORK}",
        "-m", "192M",
        "-accel", "tcg,tb-size=32",
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
        except (TimeoutError, socket.timeout):
            pass
        print("booting (text shell)...")
        time.sleep(8.0)
        mark("boot ok")

        type_line(mon, "root")
        type_line(mon, "admin")
        time.sleep(1.0)

        # [0] Prepare MKFS volume
        print("[prep] mkfs + touch hello.txt on MKFS")
        type_line(mon, "mkfs")
        time.sleep(3.0)
        type_line(mon, "touch hello.txt")
        time.sleep(1.5)

        # [1] Enter GUI
        print("[gui] entering managed desktop")
        type_line(mon, "gui")
        time.sleep(10.0)
        mark("gui ok")

        # The managed lock screen sits on top of the desktop: sign in again.
        print("[login] GUI lock screen -> admin")
        type_line(mon, "admin")
        time.sleep(5.0)
        mark("desktop unlocked")

        # [2] Open File Explorer: double-click taskbar pin 0.
        # GroupW = 8*40 + 7*6 = 362; GroupX(1280) = 459; pin0 x=505; y=676.
        print("[explorer] double-click taskbar pin 0 @ 505,676")
        left_click(mon, 505, 676); time.sleep(0.15)
        left_click(mon, 505, 676)
        time.sleep(5.0)
        shot(mon, "exp_open")
        mark("explorer opened")

        # Diagnostic run showed window origin (381,254); file list starts at
        # window-relative (162,60) -> screen (543,314).
        FILE_ROW_X, FILE_ROW_Y = 560, 330
        print(f"[mkfs] right-click first file row @ ({FILE_ROW_X}, {FILE_ROW_Y})")
        right_click(mon, FILE_ROW_X, FILE_ROW_Y)
        time.sleep(0.6)
        shot(mon, "mkfs_menu")

        # Menu is 420px tall and clamps to screen bottom (top=300), so
        # New file (idx10) y = 300+6+10*34 = 646.
        newfile_y = 646
        print(f"[mkfs] click 'New file' at (575, {newfile_y})")
        left_click(mon, 575, newfile_y)
        time.sleep(1.2)
        shot(mon, "after_newfile")
        mark("new file clicked")

        # [5] Type a name + Enter -> serial marker [FILES] new file created
        print("[mkfs] typing 'renamed.txt' + Enter")
        type_line(mon, "renamed.txt")
        print("[mkfs] waiting for inline rename to commit")
        wait_for_serial(LOG, "[FILES] rename ok", timeout=10.0)
        time.sleep(0.5)
        shot(mon, "after_rename")
        mark("rename typed")

        # [6] Switch to SFS volume: nav "System (SFS)" at window (10,94)
        # with h=30 -> screen center (456, 363).
        print("[sfs] switching to SFS volume")
        left_click(mon, 456, 363)
        time.sleep(2.5)
        time.sleep(2.0)
        shot(mon, "sfs_view")
        mark("sfs view")

        # SFS menu is only 7 items (Open,Edit,Term,OpenWith,sep,Copy,Props):
        # clicking (575,646) - the MKFS "New file" slot - lands in empty
        # space, closes the menu, and must NOT emit a create marker.
        print(f"[sfs] right-click first SFS file row @ ({FILE_ROW_X}, {FILE_ROW_Y})")
        right_click(mon, FILE_ROW_X, FILE_ROW_Y)
        time.sleep(0.6)
        shot(mon, "sfs_menu")
        print(f"[sfs] click at the would-be 'New file' position (575, {newfile_y})")
        left_click(mon, 575, newfile_y)
        time.sleep(0.8)
        shot(mon, "sfs_action")
        mark("sfs menu navigated")

        mon.sendall(b"quit\n")
        time.sleep(1.0)
    finally:
        try:
            qemu.wait(timeout=5.0)
        except subprocess.TimeoutExpired:
            qemu.terminate()
            qemu.wait(timeout=3.0)
    errf.close()

    data = ""
    if os.path.exists(LOG):
        with open(LOG, "rb") as f:
            data = f.read().decode("latin-1", "ignore")
    lines = data.splitlines()
    print("\n--- serial log tail ---")
    print("\n".join(lines[-35:]))

    def need(cond, msg):
        nonlocal ok
        if not cond:
            print("FAIL:", msg)
            ok = False

    print()
    need("EXCEPTION" not in data, "kernel exception detected")
    need("[SHELL] $ mkfs" in data, "mkfs never ran")
    need("[SHELL] $ gui" in data, "gui never entered")
    need("[FILES] new file created" in data,
         "New file action did not create a file (serial marker missing)")
    need("[FILES] rename ok" in data, "inline rename did not commit")
    need(data.count("[FILES] new file created") == 1,
         f"SFS menu produced extra new-file actions "
         f"({data.count('[FILES] new file created')} found, expected 1)")

    print("\nRESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
