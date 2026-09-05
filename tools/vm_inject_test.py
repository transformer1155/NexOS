#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
vm_inject_test.py  -  Inject local binaries into the running NexOS and test them.

The kernel only reads files that live inside the SFS volume, and SFS is a
FLAT namespace with a 19-character name ceiling (no directories).  So the
only way to "bring a file into the system" is to pack it into SFS and rebuild
the disk image.  This script automates exactly that, end to end:

    1. take files from the command line  (.exe .com .bat .cmd .ps1 .dll
       .sys .sh ... and .zip archives)
    2. inject them into sfs_files/  -  .zip members are extracted and
       flattened, every name is sanitised to fit the 19-char SFS ceiling
       and made collision-free
    3. regenerate sfs.img and dd it into a working copy of os.img
    4. boot the VM, log in (root / admin), and for each injected file
       actually run it inside NexOS:
           * Win32-ish (.exe .com .bat .cmd .ps1 .dll .sys) -> `run <name>`
             (the kernel launches it through the Win32/GUI loader)
           * shell scripts (.sh)                           -> `runfs <name>`
           * archives (.zip)                              -> `catfs <name>`
             to prove the bytes really landed in SFS
    5. print a per-file verdict derived from the serial log

Everything runs inside WSL (QEMU is Linux-only); Windows paths passed on the
command line are converted with wslpath automatically.

Usage:
    # from WSL
    python3 tools/vm_inject_test.py /path/to/app.exe other.bat

    # from Windows (file lives on the Windows side)
    wsl -e bash -lc 'cd /mnt/d/MyOS/bootloader && \
        python3 tools/vm_inject_test.py "C:\\Users\\me\\evil.exe"'

    # keep the injected files in sfs_files/ afterwards (do not auto-clean)
    python3 tools/vm_inject_test.py --keep app.exe

    # use a custom base image
    python3 tools/vm_inject_test.py --image build/os.img app.exe

    # HANDS-ON: inject, then open a real QEMU window and drive it yourself
    # (needs WSLg / an X or Wayland display). No automation, no verdict.
    python3 tools/vm_inject_test.py --gui app.exe
"""
import os
import sys
import subprocess
import socket
import time
import shutil
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)

WORK = "build/inject.img"
LOG = "build/serial_inject.log"
PORT = 4456
SFS_DIR = "sfs_files"
SFS_IMG = "build/sfs.img"
SFS_ALT_LBA = 3368          # matches SFS_ALT_LBA in kernel.cpp
MONITOR_ERR = "build/qemu_inject.err"

# ---- name policy: SFS ceiling is 19 chars, flat, ASCII -------------------
MAX_SFS_NAME = 19


def to_unix_path(p):
    """Convert a Windows path ('C:\\..' or '\\\\host\\..') to a WSL path.
    Absolute POSIX paths (start with '/') and relative paths are used as-is."""
    if len(p) >= 2 and p[1] == ':' and p[2] in ('\\', '/'):   # C:\... or C:/...
        try:
            out = subprocess.run(["wslpath", "-u", p], capture_output=True,
                                 text=True, timeout=10)
            if out.returncode == 0 and out.stdout.strip() and os.path.exists(out.stdout.strip()):
                return out.stdout.strip()
        except Exception:
            pass
        return p
    if p.startswith("\\\\"):                                   # \\host\share
        try:
            out = subprocess.run(["wslpath", "-u", p], capture_output=True,
                                 text=True, timeout=10)
            if out.returncode == 0 and out.stdout.strip() and os.path.exists(out.stdout.strip()):
                return out.stdout.strip()
        except Exception:
            pass
        return p
    return p


def sanitize_name(name):
    """Turn any filename into a safe SFS name: ascii, no spaces, lower-cased,
    dot preserved for the extension.  Not yet length- or collision-limited."""
    if os.path.sep in name:
        name = name.rsplit(os.path.sep, 1)[-1]
    b = []
    for ch in name:
        if ch in '\\/:*?"<>| ':
            b.append('_')
        elif 32 <= ord(ch) < 127:
            b.append(ch.lower())
        # drop anything non-ascii / control
    s = "".join(b)
    if not s:
        s = "file"
    return s


def fit_name(desired, used):
    """Truncate to MAX_SFS_NAME (preserving the extension) and guarantee the
    result is not already in `used`; if it is, shave and append a counter."""
    dot = desired.rfind('.')
    if dot > 0 and len(desired) - dot <= 9:
        stem, ext = desired[:dot], desired[dot:]
    else:
        stem, ext = desired, ""
    max_stem = MAX_SFS_NAME - len(ext)
    if max_stem < 1:
        ext = ext[:max_stem - 1] if max_stem > 1 else ""
        max_stem = MAX_SFS_NAME - len(ext)
    stem = stem[:max_stem]
    base = stem + ext
    if base not in used:
        used.add(base)
        return base
    i = 1
    while True:
        suff = str(i)
        room = MAX_SFS_NAME - len(ext) - len(suff)
        if room < 1:
            ext = ""
            room = MAX_SFS_NAME - len(suff)
            cand = stem[:room] + suff
        else:
            cand = stem[:room] + suff + ext
        if cand not in used:
            used.add(cand)
            return cand
        i += 1


WIN_EXTS = {".exe", ".com", ".bat", ".cmd", ".ps1", ".dll", ".sys"}
SCRIPT_EXTS = {".sh"}


def inject_file(src, sfs_dir, used, added):
    """Copy a single local file into sfs_dir under a safe SFS name.
    Returns (sfs_name, display_src) or None if it cannot be read."""
    if not os.path.isfile(src):
        print(f"  ! skip (not a file): {src}")
        return None
    # Avoid copying a file onto itself (e.g. the user passed a path that is
    # already inside sfs_files/).  If it already carries a valid SFS name we
    # just register it as injected instead of erroring out.
    if os.path.abspath(src) == os.path.abspath(os.path.join(sfs_dir, os.path.basename(src))):
        base = os.path.basename(src)
        if len(base) <= MAX_SFS_NAME and base not in used:
            used.add(base); added.append(base)
            print(f"  = {base}  (already in SFS, using as-is)")
            return base
    safe = fit_name(sanitize_name(os.path.basename(src)), used)
    dst = os.path.join(sfs_dir, safe)
    shutil.copyfile(src, dst)
    added.append(safe)
    print(f"  + {os.path.basename(src)}  ->  sfs:{safe}")
    return safe


def inject_zip(src, sfs_dir, used, added):
    """Extract a .zip's members (flattened) into sfs_dir."""
    if not os.path.isfile(src):
        print(f"  ! skip (not a file): {src}")
        return []
    names = []
    try:
        with zipfile.ZipFile(src) as z:
            for info in z.infolist():
                if info.is_dir():
                    continue
                member = info.filename.rsplit('/', 1)[-1].rsplit('\\', 1)[-1]
                if not member:
                    continue
                safe = fit_name(sanitize_name(member), used)
                try:
                    data = z.read(info)
                except Exception as e:
                    print(f"  ! cannot read zip member {member}: {e}")
                    continue
                with open(os.path.join(sfs_dir, safe), "wb") as f:
                    f.write(data)
                added.append(safe)
                names.append(safe)
                print(f"  + (zip) {member}  ->  sfs:{safe}")
    except Exception as e:
        print(f"  ! cannot open zip {src}: {e}")
    return names


def rebuild_sfs():
    """Regenerate sfs.img from sfs_dir and splice it into the work image."""
    subprocess.run([sys.executable, "tools/sfs_gen.py", SFS_DIR, SFS_IMG],
                   check=True)
    subprocess.run(["dd", "if=" + SFS_IMG, "of=" + WORK,
                    "bs=512", f"seek={SFS_ALT_LBA}", "conv=notrunc"],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def wait_sock(port, timeout=30.0):
    end = time.time() + timeout
    while time.time() < end:
        try:
            return socket.create_connection(("127.0.0.1", port), timeout=0.5)
        except OSError:
            time.sleep(0.2)
    raise RuntimeError("QEMU monitor did not come up on port %d" % port)


def send_key(mon, key, delay=0.12):
    mon.sendall(f"sendkey {key}\n".encode())
    time.sleep(delay)


def type_line(mon, s, delay=0.09):
    keymap = {' ': 'spc', '.': 'dot', '/': 'slash', '\\': 'backslash',
              '-': 'minus', '_': 'shift-minus'}
    for ch in s:
        key = f"shift-{ch.lower()}" if 'A' <= ch <= 'Z' else keymap.get(ch, ch)
        send_key(mon, key, delay)
    send_key(mon, "ret", 0.4)


def main():
    args = sys.argv[1:]
    keep = False
    gui = False
    image = "build/os.img"
    files = []
    i = 0
    while i < len(args):
        a = args[i]
        if a == "--keep":
            keep = True
        elif a == "--gui":
            gui = True
            keep = True          # you are driving it by hand; keep the files
        elif a == "--image":
            image = args[i + 1]; i += 1
        elif a.startswith("--"):
            print(f"unknown option: {a}")
            sys.exit(2)
        else:
            files.append(a)
        i += 1

    if not files:
        print("usage: vm_inject_test.py [--keep] [--gui] [--image IMG] "
              "FILE [FILE ...]")
        sys.exit(2)

    # ---- 1. stage the files into SFS -------------------------------------
    sfs_dir = os.path.join(ROOT, SFS_DIR)
    os.makedirs(sfs_dir, exist_ok=True)
    os.makedirs("build", exist_ok=True)

    used = set()
    added = []          # SFS names we created (for cleanup)
    injected = []       # (sfs_name, ext) that should be exercised
    print("Injecting files into SFS:")
    for f in files:
        f = to_unix_path(f)
        ext = os.path.splitext(f)[1].lower()
        if ext == ".zip":
            for n in inject_zip(f, sfs_dir, used, added):
                injected.append((n, os.path.splitext(n)[1].lower()))
        else:
            r = inject_file(f, sfs_dir, used, added)
            if r:
                injected.append((r, ext))

    if not injected:
        print("nothing injectable was provided; aborting")
        sys.exit(1)

    # ---- 2. rebuild the disk image ---------------------------------------
    print(f"Rebuilding SFS and splicing into {WORK} ...")
    shutil.copyfile(image, WORK)
    rebuild_sfs()

    # ---- 3a. hands-on mode: open a real window and hand over the keyboard --
    if gui:
        print("\n" + "=" * 62)
        print("  INTERACTIVE SESSION - the files below are now inside NexOS")
        print("=" * 62)
        for name, ext in injected:
            print(f"    {name}")
        print("\n  login : root / admin")
        print("  try   : ls            list the SFS")
        for name, ext in injected:
            if ext in WIN_EXTS:
                print(f"          run {name}")
            elif ext in SCRIPT_EXTS:
                print(f"          runfs {name}")
            else:
                print(f"          catfs {name}")
        print("          user          run the ring-3 sandbox demo")
        print("          perm          show remembered permission grants")
        print("  quit  : close the QEMU window")
        print("=" * 62 + "\n")
        rc = subprocess.call([
            "qemu-system-x86_64",
            "-drive", f"format=raw,file={WORK}",
            "-m", "128M",              # ring-3 region is at 64-128 MiB
            "-vga", "std",
            "-display", "gtk",
            "-no-reboot",
            "-serial", f"file:{LOG}",
        ])
        print(f"\nQEMU exited (rc={rc}). Serial log: {LOG}")
        print(f"Injected files were kept in {SFS_DIR}/ "
              f"({', '.join(n for n, _ in injected)}).")
        sys.exit(0)

    # ---- 3. boot + run ---------------------------------------------------
    # GUI mode is modal: once `run foo.exe` enters the GUI the text shell
    # stops reading the keyboard, so every later command would be swallowed.
    # We therefore split the run into groups: all the non-GUI checks (plain
    # SFS presence, shell scripts) share one boot, and each Win32/GUI file
    # gets its own boot so its launch can be observed in isolation.
    boot_nongui = [t for t in injected if t[1] not in WIN_EXTS]
    boot_gui = [t for t in injected if t[1] in WIN_EXTS]

    logs = []          # (label, serial_text) per boot, for the final verdict

    def boot_and_type(plan, wait_after):
        """Boot once, log in, run the `plan` list of (command, name) pairs,
        capture the serial log, return its text. `plan` may be empty (we still
        boot+login just to confirm the image is alive)."""
        for f in (LOG, MONITOR_ERR):
            if os.path.exists(f):
                os.remove(f)
        errf = open(MONITOR_ERR, "wb")
        q = subprocess.Popen([
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
        try:
            mon = wait_sock(PORT)
            mon.settimeout(3.0)
            try:
                mon.recv(65536)
            except (TimeoutError, socket.timeout):
                pass
            time.sleep(8.0)
            type_line(mon, "root")
            type_line(mon, "admin")
            time.sleep(1.0)
            for cmd, _ in plan:
                print(f"\n[SHELL] {cmd}")
                type_line(mon, cmd)
                time.sleep(wait_after)
            mon.sendall(b"quit\n")
            time.sleep(1.0)
        finally:
            try:
                q.wait(timeout=5.0)
            except subprocess.TimeoutExpired:
                q.terminate()
                q.wait(timeout=3.0)
        errf.close()
        with open(LOG, "rb") as f:
            return f.read().decode("latin-1", "ignore")

    # Group 1: non-GUI checks in a single boot.
    if boot_nongui:
        plan = []
        for name, ext in boot_nongui:
            if ext in SCRIPT_EXTS:
                plan.append((f"runfs {name}", name))
            else:
                plan.append((f"catfs {name}", name))
        logs.append(("non-GUI", boot_and_type(plan, 2.5)))

    # Group 2: each Win32/GUI file in its own boot.
    for name, ext in boot_gui:
        logs.append((name, boot_and_type([(f"run {name}", name)], 3.0)))

    # ---- 4. verdict ------------------------------------------------------
    # Each injected file is judged against the boot log that actually exercised
    # it (the non-GUI group, or its own GUI boot).
    print("\n" + "=" * 60)
    print("INJECTION TEST RESULTS")
    print("=" * 60)
    ok = True
    nongui_data = ""
    gui_data = {name: txt for name, txt in logs if name != "non-GUI"}
    for name, txt in logs:
        if name == "non-GUI":
            nongui_data = txt

    for name, ext in injected:
        if ext in WIN_EXTS:
            data = gui_data.get(name, "")
            dispatched = (f"[GUI] Initializing on demand for: {name}" in data) or \
                         (f"Launching in GUI: {name}" in data) or \
                         (f"[WIN32]" in data and name in data)
            notfound = f"File not found: {name}" in data
            launched = f"[WIN32] calling PE entry" in data
            cat = "Win32/GUI loader"
            if dispatched and not notfound:
                mark, status = ("LAUNCHED" if launched else "DISPATCHED"), "ok"
            else:
                mark, status = "NOT FOUND IN SFS", "FAIL"
            ok = ok and (status == "ok")
        elif ext in SCRIPT_EXTS:
            launched = f"--- running: {name}" in nongui_data or f"--- running SFS: {name}" in nongui_data
            cat = "shell script"
            mark = "RAN" if launched else "NOT RUN"
            status = "ok" if launched else "FAIL"
            ok = ok and launched
        else:
            notfound = f"File not found: {name}" in nongui_data
            cat = "archived/other"
            mark = "IN SFS" if not notfound else "MISSING"
            status = "ok" if not notfound else "FAIL"
            ok = ok and (not notfound)
        print(f"  [{status:4}] {name:20} ({cat:14}) -> {mark}")

    print("\nRESULT:", "PASS" if ok else "FAIL")
    if any("EXCEPTION" in txt for _, txt in logs):
        print("WARNING: kernel EXCEPTION seen in a serial log")

    # ---- 6. cleanup (leave sfs_files/ AND the images as we found them) ----
    if not keep:
        # Never delete files that were already part of the base image before
        # we started (e.g. hello32.exe).  Only what *we* injected is removed.
        base_before = set(os.listdir(sfs_dir)) - set(added)
        for n in added:
            if n in base_before:
                print(f"  ! refusing to remove pre-existing base file: {n}")
                continue
            try:
                os.remove(os.path.join(sfs_dir, n))
            except OSError:
                pass
        # restore sfs.img and re-splice it into build/os.img so a later `make`
        # (or another test) does not inherit our injected files.
        try:
            subprocess.run([sys.executable, "tools/sfs_gen.py", SFS_DIR, SFS_IMG],
                           check=True, stdout=subprocess.DEVNULL)
            subprocess.run(["dd", "if=" + SFS_IMG, "of=build/os.img",
                            "bs=512", f"seek={SFS_ALT_LBA}", "conv=notrunc"],
                           check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        except Exception:
            pass

    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
