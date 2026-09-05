#!/usr/bin/env python3
"""Boot os.img headless, capture serial, print ATA-diag + SFS-mount lines."""
import os, time, socket, subprocess

PROJ = r"D:\MyOS\bootloader"
BUILD = os.path.join(PROJ, "build")
QEMU = r"D:\qemu\qemu-system-x86_64.exe"
DISK = os.path.join(BUILD, "os.img")
SERIAL = os.path.join(BUILD, "serial_ata.txt")
MON_PORT = 4591

def build_cmd():
    return [QEMU, "-machine", "pc", "-m", "256", "-accel", "tcg,tb-size=128",
            "-drive", f"file={DISK},format=raw,if=ide",
            "-device", "loader,addr=0x501E,data=1,data-len=1",
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

def mon(cmd, retries=4):
    for _ in range(retries):
        try:
            s = socket.create_connection(("127.0.0.1", MON_PORT), timeout=5)
            s.sendall((cmd + "\n").encode())
            time.sleep(0.3); s.close(); return True
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

def main():
    kill_stale()
    open(SERIAL, "w").close()
    proc = subprocess.Popen(build_cmd(),
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if not wait_mon(30):
        print("[ERR] monitor never up"); proc.kill(); return
    if proc.poll() is not None:
        print(f"[ERR] QEMU exited early {proc.returncode}"); return
    print("[BOOT] waiting for 64-bit SFS mount ...")
    time.sleep(30)
    if proc.poll() is not None:
        print(f"[ERR] QEMU exited early {proc.returncode}")
    mon("quit")
    try:
        proc.wait(timeout=10)
    except Exception:
        proc.kill()
    with open(SERIAL, "rb") as f:
        txt = f.read().decode("utf-8", "ignore")
    wanted = ("ATA-DIAG", "K64-6", "SFS mounted", "SFS NOT", "step=318",
              "step=319", "shell.mex ready", "vec_init", "VECPEEK", "VECERR")
    for line in txt.splitlines():
        if any(w in line for w in wanted):
            print(line)
    print("[DONE]")

if __name__ == "__main__":
    main()
