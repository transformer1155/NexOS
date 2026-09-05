#!/usr/bin/env python3
"""Headless disk-write verification for NexOS.

Boots build/os.img in TEXT mode (-vga none, so the shell is reachable via
serial instead of the GUI eating the keyboard), logs in, then drives:

  [1] disk        - disk + SFS info (model / sectors / free_lba)
  [2] disk rw     - raw ATA sector round-trip: write a patterned sector to
                    the SFS free area, read it back, byte-verify.
                    PASS marker: [DISKRW] PASS
  [3] disk sfs    - SFS file round-trip: create a 1600-byte file (3+ sectors)
                    -> read back -> byte-verify -> rename -> verify -> remove.
                    PASS marker: [DISKSFS] ALL PASS
  [4] lsfs        - confirm rwtest files are gone from the directory listing.

Also asserts NO kernel exception occurred anywhere in the log.
"""
import os, sys, socket, time, subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)

IMG = sys.argv[1] if len(sys.argv) > 1 else "build/os.img"
WORK = "build/disk_rw.img"          # copy: the test WRITES to the disk image
LOG = "build/serial_disk_rw.log"
PORT = 4471


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
    time.sleep(0.4)


def main():
    subprocess.run(["cp", IMG, WORK], check=True)
    if os.path.exists(LOG):
        os.remove(LOG)
    errf = open("build/qemu_disk_rw.err", "wb")
    qemu = subprocess.Popen([
        r"D:\qemu\qemu-system-x86_64.exe",
        "-drive", f"format=raw,file={WORK}",
        "-m", "128M",
        "-vga", "none",          # text shell reachable via serial
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
        print("[login] root / admin")
        type_line(mon, "root")
        type_line(mon, "admin")
        time.sleep(1.0)

        print("[1] disk  (info)")
        type_line(mon, "disk")
        time.sleep(1.0)

        print("[2] disk rw  (raw ATA round-trip)")
        type_line(mon, "disk rw")
        time.sleep(2.0)

        print("[3] disk sfs  (SFS file round-trip)")
        type_line(mon, "disk sfs")
        time.sleep(3.0)

        print("[4] lsfs  (cleanup check)")
        type_line(mon, "lsfs")
        time.sleep(1.5)

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
        print("ERROR: serial log was never created. QEMU stderr:")
        with open("build/qemu_disk_rw.err", "rb") as f:
            print(f.read().decode("latin-1", "ignore")[:2000])
        return 1

    with open(LOG, "rb") as f:
        data = f.read().decode("latin-1", "ignore")
    lines = data.splitlines()
    print("\n--- serial log tail ---")
    print("\n".join(lines[-40:]))

    ok = True

    def need(cond, msg):
        nonlocal ok
        if not cond:
            print("FAIL:", msg)
            ok = False

    print()
    need("EXCEPTION" not in data, "kernel exception detected")
    need("TRIPLE" not in data.upper(), "triple fault detected")

    # [1] info
    need("[SHELL] $ disk" in data, "`disk` never reached the shell")
    need("SFS @ LBA" in data, "`disk` did not show SFS mount info")
    need("free_lba=" in data, "`disk` did not show free_lba")

    # [2] raw round-trip
    need("[SHELL] $ disk rw" in data, "`disk rw` never reached the shell")
    need("[DISKRW] sector written" in data, "raw sector write did not happen")
    need("[DISKRW] PASS" in data, "raw ATA round-trip FAILED")
    need("[DISKRW] FAIL" not in data, "raw ATA round-trip reported FAIL")

    # [3] SFS file round-trip
    need("[SHELL] $ disk sfs" in data, "`disk sfs` never reached the shell")
    need("[DISKSFS] create ok" in data, "SFS create failed")
    need("[DISKSFS] read PASS" in data, "SFS read-back verification failed")
    need("[DISKSFS] rename PASS" in data, "SFS rename failed")
    need("[DISKSFS] remove PASS" in data, "SFS remove failed")
    need("[DISKSFS] ALL PASS" in data, "SFS round-trip did not reach ALL PASS")
    need("[DISKSFS] FAIL" not in data, "SFS round-trip reported FAIL")

    # [4] cleanup check: neither rwtest file may appear in lsfs
    need("[SHELL] $ lsfs" in data, "`lsfs` never reached the shell")
    need("rwtest" not in data.split("[SHELL] $ lsfs", 1)[1],
         "rwtest files still listed after remove")

    print("\nRESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
