#!/usr/bin/env python3
"""Headless verify of the 64-bit ata_read_sector fix.

Boots os_textboot.img (TEXT_BOOT -> text shell, no auto-GUI), logs in as
admin/admin at the serial console, types `switch` to invoke cmd_switch64()
-> do_switch64() (loads the 64-bit kernel from disk via the 32-bit ATA
driver, then enters long mode).  kmain64 then runs ata_diag_probe() and
sfs.init(); we capture the serial log and look for:

  [ATA-DIAG]  lines:  LBAxxxx err=.. st=.. b0=........ cks=....
  [K64-6] SFS mounted            (or *** SFS NOT mounted ***)

Ground truth (from python over the disk image):
  LBA0    b0=FA31C08E  cks=6860   (boot sector)
  LBA800  b0=09CA8B4D  cks=...    (not SFS)
  LBA3488 b0=53465300  cks=841b   (SFS superblock 'SFS\\0')

If the 64-bit ata_read_sector were still broken (always reading LBA0),
every LBA line would show b0=FA31C08E and SFS would NOT mount.
"""
import os, time, socket, subprocess

PROJ = r"D:\MyOS\bootloader"
BUILD = os.path.join(PROJ, "build")
QEMU = r"D:\qemu\qemu-system-x86_64.exe"
DISK = os.path.join(BUILD, "os_textboot.img")
SERIAL = os.path.join(BUILD, "serial_switch.txt")
MON_PORT = 4592

def build_cmd():
    return [QEMU, "-machine", "pc", "-m", "256", "-accel", "tcg,tb-size=128",
            "-drive", f"file={DISK},format=raw,if=ide",
            "-serial", f"file:{SERIAL}",
            "-monitor", f"tcp:127.0.0.1:{MON_PORT},server,nowait",
            "-display", "none", "-vga", "std"]

def kill_stale():
    try:
        subprocess.run(["taskkill", "/F", "/IM", "qemu-system-x86_64.exe"],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except Exception:
        pass
    time.sleep(1.0)

def read_serial():
    try:
        with open(SERIAL, "rb") as f:
            return f.read().decode("utf-8", "ignore")
    except Exception:
        return ""

def serial_has(sub):
    return sub in read_serial()

def mon(cmd, retries=6):
    for _ in range(retries):
        try:
            s = socket.create_connection(("127.0.0.1", MON_PORT), timeout=5)
            s.sendall((cmd + "\n").encode())
            time.sleep(0.4); s.close(); return True
        except Exception:
            time.sleep(1.0)
    return False

def wait_mon(timeout=30):
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            s = socket.create_connection(("127.0.0.1", MON_PORT), timeout=2)
            s.close(); return True
        except Exception:
            time.sleep(0.5)
    return False

def wait_for(sub, timeout, label):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if serial_has(sub):
            print(f"[OK] saw {label!r} after {time.time()-t0:.1f}s")
            return True
        time.sleep(0.5)
    print(f"[TIMEOUT] never saw {label!r} in {timeout}s")
    return False

def type_text(s):
    """Type a string into the VM by sending one QEMU key event per char."""
    for ch in s:
        if not mon(f"sendkey {ch}"):
            print(f"[WARN] sendkey {ch} failed")
        time.sleep(0.12)

def enter():
    mon("sendkey ret")
    time.sleep(0.25)

def main():
    if not os.path.exists(DISK):
        print(f"[ERR] {DISK} missing -- run: make textboot")
        return
    kill_stale()
    open(SERIAL, "w").close()
    proc = subprocess.Popen(build_cmd(),
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if not wait_mon(30):
        print("[ERR] monitor never up"); proc.kill(); return
    if proc.poll() is not None:
        print(f"[ERR] QEMU exited early {proc.returncode}"); return

    print("[BOOT] waiting for login prompt ...")
    if not wait_for("login: ", 60, "login:"):
        print(read_serial()[-1500:]); mon("quit"); return

    type_text("root"); enter()
    if not wait_for("Password: ", 20, "Password:"):
        print(read_serial()[-1500:]); mon("quit"); return

    type_text("admin"); enter()
    if not wait_for("Welcome, root", 20, "Welcome, root"):
        print(read_serial()[-1500:]); mon("quit"); return

    print("[SHELL] logged in; typing 'switch' -> 64-bit kernel")
    time.sleep(1.0)
    type_text("switch"); enter()

    print("[WAIT] waiting for kmain64 ata_diag_probe + SFS mount ...")
    # Poll up to 40s for the diag block (covers staging read + long-mode entry).
    if not wait_for("[ATA-DIAG] done", 45, "[ATA-DIAG] done"):
        print("[NOTE] ata-diag not seen; dumping tail")
        print(read_serial()[-2000:])

    time.sleep(20)
    mon("quit")
    try:
        proc.wait(timeout=10)
    except Exception:
        proc.kill()

    txt = read_serial()
    print("=" * 60)
    print("RELEVANT SERIAL LINES")
    print("=" * 60)
    wanted = ("[K32]", "[ATA-DIAG]", "LBA", "b0=", "cks=", "[K64-6]",
              "SFS mounted", "SFS NOT", "switch", "Switching", "entry64",
              "long mode", "kmain64", "vec_init", "VECPEEK", "VECERR",
              "err=", "mkfs", "MKFS", "mount", "pmm_init", "vmm_init",
              "[K64-7]", "[K64-8]", "shell.mex", "shell", "nexos", "NexOS")
    for line in txt.splitlines():
        if any(w in line for w in wanted):
            print(line)
    print("=" * 60)
    print("--- FULL SERIAL TAIL (last 4000 chars) ---")
    print(txt[-4000:])
    # Quick verdict
    if "b0=53465300" in txt:
        print("[VERDICT] LBA3488 (SFS superblock) read correctly -> ata fix OK")
    else:
        print("[VERDICT] LBA3488 SFS magic NOT seen -> ata still broken")
    if "SFS mounted" in txt:
        print("[VERDICT] 64-bit SFS mounted -> vec_init path reachable")
    print("[DONE]")

if __name__ == "__main__":
    main()
