#!/usr/bin/env python3
"""Debug run: boot textboot, run `linux busybox echo hello nexos`, wait, then
dump FULL QEMU registers + disassembly at the halt EIP."""
import os, sys, socket, time, subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)

IMG = "build/os_textboot.img"
WORK = "build/bb_dbg.img"
LOG = "build/serial_bb_dbg.log"
PORT = 4491

QEMU = r"D:\qemu\qemu-system-x86_64.exe"

def wait_sock(port, timeout=30.0):
    end = time.time() + timeout
    while time.time() < end:
        try:
            return socket.create_connection(("127.0.0.1", port), timeout=0.5)
        except OSError:
            time.sleep(0.2)
    raise RuntimeError("monitor not ready")

def type_line(mon, s):
    keymap = {' ': 'spc', '.': 'dot', '/': 'slash', '-': 'minus', ':': 'shift-semicolon'}
    for ch in s:
        key = f"shift-{ch.lower()}" if 'A' <= ch <= 'Z' else keymap.get(ch, ch)
        mon.sendall(f"sendkey {key}\n".encode())
        time.sleep(0.06)
    mon.sendall(b"sendkey ret\n")
    time.sleep(0.3)

def mon_cmd(mon, cmd, wait=0.6):
    mon.sendall((cmd + "\n").encode())
    time.sleep(wait)
    out = b""
    mon.settimeout(1.0)
    try:
        while True:
            chunk = mon.recv(65536)
            if not chunk: break
            out += chunk
    except socket.timeout:
        pass
    return out.decode("latin-1", "ignore")

def main():
    subprocess.run(["cp", IMG, WORK], check=True)
    try: os.remove(LOG)
    except OSError: pass
    errf = open("build/qemu_bb_dbg.err", "wb")
    qemu = subprocess.Popen([
        QEMU, "-machine", "pc",
        "-drive", f"format=raw,file={WORK},if=ide",
        "-m", "256M", "-accel", "tcg,tb-size=128",
        "-vga", "std", "-display", "none", "-no-reboot",
        "-monitor", f"tcp:127.0.0.1:{PORT},server,nowait",
        "-chardev", f"file,id=ser,path={LOG}",
        "-serial", "chardev:ser",
    ], stdout=errf, stderr=errf)
    try:
        mon = wait_sock(PORT)
        mon.settimeout(3.0)
        try: mon.recv(65536)
        except (TimeoutError, socket.timeout): pass
        time.sleep(7.0)
        type_line(mon, "root")
        type_line(mon, "admin")
        time.sleep(1.0)
        type_line(mon, "linux busybox echo hello nexos")
        time.sleep(12.0)
        print("===== FULL REGISTERS =====")
        print(mon_cmd(mon, "info registers"))
        print("===== DISASM AT EIP =====")
        print(mon_cmd(mon, "x/12i 0x080fb6c0"))
        print("===== STACK 32 WORDS =====")
        print(mon_cmd(mon, "xp/32wx 0x0814e000"))
        mon.sendall(b"quit\n")
        time.sleep(0.5)
    finally:
        try: qemu.wait(timeout=5.0)
        except subprocess.TimeoutExpired:
            qemu.terminate(); qemu.wait(timeout=3.0)
    errf.close()
    with open(LOG, "rb") as f:
        data = f.read().decode("latin-1", "ignore")
    lines = data.splitlines()
    print("\n===== SERIAL TAIL =====")
    print("\n".join(lines[-30:]))

if __name__ == "__main__":
    main()
