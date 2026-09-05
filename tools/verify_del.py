#!/usr/bin/env python3
"""End-to-end verification of the desktop / file-manager DELETE fix.

Root cause (fixed in mforms.cpp::h_file_name): file names that contain spaces
(e.g. desktop shortcuts "This PC.lnk", "Task Mgr.lnk", or the file manager's
auto-named "New File.txt") were truncated at the first space, so Host.FileDelete
looked up a bogus name ("This", "Task", "New") and mkfs.remove silently no-op'd.

This script boots the REAL VM (os.img), signs in, and drives the QEMU HMP mouse
to right-click two space-bearing desktop shortcuts and pick "Delete" from the
context menu.  Success is proven by the kernel serial marker that gui_cb_remove
now emits on every delete:

    [DEL] fs=3 name='<FULL NAME>' r=<code>

where r=0 (or >=0) means mkfs.remove actually removed the file.  Seeing the FULL
space-bearing name there proves h_file_name is no longer truncating, which is the
exact same code path the file manager (fs==0) uses -- so the fix covers both
complaints with one change.
"""
import os, sys, time, socket, subprocess

PROJ  = r"D:\MyOS\bootloader"
BUILD = os.path.join(PROJ, "build")
QEMU  = r"D:\qemu\qemu-system-x86_64.exe"
DISK  = os.path.join(BUILD, "os.img")
SERIAL = os.path.join(BUILD, "serial_del.txt")
MON   = 4593

for f in (SERIAL,):
    if os.path.exists(f):
        os.remove(f)


def kill_stale():
    try:
        subprocess.run(["taskkill", "/F", "/IM", "qemu-system-x86_64.exe"],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except Exception:
        pass
    time.sleep(1.0)


def mon_connect(retries=30):
    for _ in range(retries):
        try:
            s = socket.create_connection(("127.0.0.1", MON), timeout=5)
            s.settimeout(2.0)
            return s
        except Exception:
            time.sleep(0.5)
    return None


def mcmd(s, c, wait=0.3):
    s.sendall((c + "\n").encode())
    time.sleep(wait)


def read_line(s):
    buf = b""
    while True:
        try:
            d = s.recv(1)
        except socket.timeout:
            return buf.decode(errors="replace")
        if not d:
            return buf.decode(errors="replace")
        buf += d
        if d == b"\n":
            return buf.decode(errors="replace")


def mresp(s, c, wait=0.3):
    mcmd(s, c, wait)
    out = []
    for _ in range(8):
        ln = read_line(s)
        if not ln:
            continue
        out.append(ln.rstrip("\n"))
        if ln.startswith("(qemu)"):
            break
    return out


def mouse_move(s, x, y):
    # QEMU HMP mouse_move is RELATIVE; snap to origin first, then move to (x,y).
    mresp(s, "mouse_move -10000 -10000", 0.08)
    mresp(s, f"mouse_move {x} {y}", 0.1)


def right_click(s, x, y):
    mouse_move(s, x, y); time.sleep(0.2)
    mresp(s, "mouse_button 2", 0.12)   # press right
    mresp(s, "mouse_button 0", 0.3)    # release


def left_click(s, x, y):
    mouse_move(s, x, y); time.sleep(0.2)
    mresp(s, "mouse_button 1", 0.12)   # press left
    mresp(s, "mouse_button 0", 0.3)    # release


def type_keys(s, text):
    # Match capture_desk.py's 0.4s/key pacing: the lock-screen login form
    # (CLR) processes keys on a slow tick, so faster sends drop characters
    # and the password never matches -> login silently fails.
    for ch in text:
        mresp(s, f"sendkey {ch}", 0.4)
    mresp(s, "sendkey ret", 0.5)
    mresp(s, "sendkey return", 0.5)


def serial_tail(n=6000):
    try:
        with open(SERIAL, "rb") as f:
            f.seek(0, os.SEEK_END)
            sz = f.tell()
            f.seek(max(0, sz - n), os.SEEK_SET)
            return f.read().decode("utf-8", "ignore")
    except Exception:
        return ""


def main():
    kill_stale()
    open(SERIAL, "w").close()
    proc = subprocess.Popen([
        QEMU, "-machine", "pc", "-m", "256",
        "-accel", "tcg,tb-size=128",
        "-drive", f"file={DISK},format=raw,if=ide",
        "-serial", f"file:{SERIAL}",
        "-monitor", f"tcp:127.0.0.1:{MON},server,nowait",
        "-display", "none", "-vga", "std",
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    s = mon_connect()
    if s is None:
        print("[ERR] monitor never came up")
        proc.kill(); return
    if proc.poll() is not None:
        print(f"[ERR] QEMU exited early rc={proc.returncode}")
        return

    # Wait for the managed shell / lock screen
    print("[BOOT] waiting for managed shell ...")
    t0 = time.time()
    ready = False
    while time.time() - t0 < 80:
        if "shell.mex ready" in serial_tail() or "[MFORMS]" in serial_tail() or "mforms_paint_desktop" in serial_tail():
            ready = True; break
        if proc.poll() is not None:
            print(f"[ERR] QEMU exited early rc={proc.returncode}"); return
        time.sleep(1.0)
    print(f"[BOOT] shell ready = {ready}")

    # Sign in as root / admin (user pre-filled, focus on password)
    print("[AUTH] typing 'admin' + Enter ...")
    type_keys(s, "admin")
    t0 = time.time()
    desktop = False
    while time.time() - t0 < 30:
        if "mforms_paint_desktop" in serial_tail() or "[M-POLL]" in serial_tail():
            desktop = True; break
        if proc.poll() is not None:
            print(f"[ERR] QEMU exited early rc={proc.returncode}"); return
        time.sleep(0.5)
    print(f"[AUTH] desktop reached = {desktop}")
    time.sleep(5.0)   # let the desktop unlock + icons paint / settle

    # ---- Delete test 1: "Task Mgr.lnk" (index 3 => col3,row0) ----
    # gridX=223, gridY=100, tile 92x66, gap 14 -> col stride 106, row stride 80
    ix1, iy1 = 223 + 3 * 106 + 46, 100 + 0 * 80 + 33   # (587,133)
    print(f"[DEL1] right-click desktop icon 'Task Mgr.lnk' at ({ix1},{iy1})")
    right_click(s, ix1, iy1); time.sleep(0.7)
    # context menu: Open/Edit/OpenTerm/OpenWith/sep/Copy/Delete(6)/Rename/Properties
    # item height 34, top pad 6 -> Delete row center y = iy1 + 6 + 6*34 + 17
    dx1, dy1 = ix1 + 60, iy1 + 6 + 6 * 34 + 17
    print(f"[DEL1] click 'Delete' menu item at ({dx1},{dy1})")
    left_click(s, dx1, dy1); time.sleep(1.2)

    # ---- Delete test 2: "This PC.lnk" (index 0 => col0,row0) ----
    # Removing index 3 first re-packs indices >=3, but index 0 is untouched,
    # so 'This PC.lnk' stays at its original grid slot.
    ix2, iy2 = 223 + 0 * 106 + 46, 100 + 0 * 80 + 33   # (269,133)
    print(f"[DEL2] right-click desktop icon 'This PC.lnk' at ({ix2},{iy2})")
    right_click(s, ix2, iy2); time.sleep(0.7)
    dx2, dy2 = ix2 + 60, iy2 + 6 + 6 * 34 + 17
    print(f"[DEL2] click 'Delete' menu item at ({dx2},{dy2})")
    left_click(s, dx2, dy2); time.sleep(1.2)

    # Collect serial evidence
    log = serial_tail(20000)
    dels = [ln for ln in log.splitlines() if "[DEL]" in ln]
    print("\n=== SERIAL [DEL] MARKERS ===")
    for d in dels:
        print("   ", d.strip())

    # Verdict
    want = ["Task Mgr.lnk", "This PC.lnk"]
    got = [d for d in dels if "name='" in d]
    # mkfs.remove returns 0 (or any non-negative) on success; -1/-2 = not found /
    # read-only.  Any non-negative code means the file was actually removed.
    detail = []
    for w in want:
        hit = [d for d in got if w in d]
        if not hit:
            detail.append(f"{w}: NOT DELETED (no [DEL] marker)")
        else:
            d = hit[0]
            if "r=-" not in d:
                detail.append(f"{w}: DELETED ok ({d.strip()})")
            else:
                detail.append(f"{w}: marker seen but r<0 ({d.strip()})")
    print("\n=== VERDICT ===")
    for line in detail:
        print("   ", line)
    verdict = all("DELETED ok" in x for x in detail)
    print("[VERDICT]", "PASS - space-bearing names now delete correctly"
          if verdict else "CHECK - see detail above")

    try:
        mresp(s, "quit")
    except Exception:
        pass
    try:
        proc.wait(timeout=15)
    except Exception:
        try:
            proc.kill()
        except Exception:
            pass
    print("[DONE]")


if __name__ == "__main__":
    main()
