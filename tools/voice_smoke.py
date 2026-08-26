#!/usr/bin/env python3
"""Headless smoke test for the NexOS voice engine.

Boots build/os.img under QEMU (TCG), enters GUI mode via HMP sendkey, then
injects a framed voice phrase ("\x02 len utf8 -> 打开浏览器") on COM1 through a
named pipe and verifies the managed Voice engine fires (serial log shows
"VOICE:ok") without breaking the GUI bootstrap.

Serial transport note: on this QEMU 11.1.0 / Windows build, *tcp* chardevs
silently crash the VM, so we use a Windows named pipe for COM1 (stable) and a
tcp monitor only for keyboard injection.

Protocol (see kernel64.cpp serial_voice_poll):
    byte 0   = 0x02  (STX)
    byte 1   = length (1..127)
    bytes 2+ = UTF-8 phrase bytes
"""
import os, socket, time, subprocess, sys, shutil, threading

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)

IMG = "build/os.img"
WORK = "build/gui_voice.img"
PORT_MON = 4451          # QEMU monitor (sendkey, per-command reconnect)
PIPE_NAME = r"\\.\pipe\nexos_voice"
PIPE_SHORT = "nexos_voice"
QEMU = r"D:\qemu\qemu-system-x86_64.exe"

# Shared, thread-fed buffer of everything the guest wrote to COM1.
SER_BUF = bytearray()
SER_LOCK = threading.Lock()


def mon_send(cmd, retries=5):
    """Send one HMP command on a fresh monitor connection (one-shot server)."""
    for attempt in range(retries):
        try:
            s = socket.create_connection(("127.0.0.1", PORT_MON), timeout=5)
            s.sendall((cmd + "\n").encode())
            time.sleep(0.25)
            s.close()
            return True
        except Exception as e:
            if attempt + 1 >= retries:
                print(f"[MON] send failed: {e}")
                return False
            time.sleep(0.5)
    return False


def type_line(text, delay=0.10):
    for ch in text:
        if 'A' <= ch <= 'Z':
            mon_send(f"sendkey shift-{ch.lower()}")
        elif ch == ' ':
            mon_send("sendkey spc")
        else:
            mon_send(f"sendkey {ch}")
        time.sleep(delay)
    mon_send("sendkey ret")
    time.sleep(0.5)


def wait_port(port, timeout=40.0):
    end = time.time() + timeout
    while time.time() < end:
        try:
            s = socket.create_connection(("127.0.0.1", port), timeout=0.5)
            s.close()
            return True
        except OSError:
            time.sleep(0.2)
    return False


def pipe_reader(pipe):
    """Drain the named pipe into SER_BUF until EOF/error."""
    try:
        while True:
            chunk = pipe.read(4096)
            if not chunk:
                time.sleep(0.05)
                continue
            with SER_LOCK:
                SER_BUF.extend(chunk)
    except Exception:
        pass


def read_until(marker, timeout=30.0):
    end = time.time() + timeout
    while time.time() < end:
        with SER_LOCK:
            if marker in bytes(SER_BUF):
                return True
        time.sleep(0.2)
    return False


def frame(phrase):
    b = phrase.encode("utf-8")
    return b"\x02" + bytes([len(b) & 0x7F]) + b


def main():
    if not os.path.exists(IMG):
        print("ERROR: build/os.img missing; run the build first")
        return 2

    shutil.copy(IMG, WORK)
    errf = open("build/qemu_voice.err", "wb")

    qemu = subprocess.Popen([
        QEMU,
        "-drive", f"format=raw,file={WORK}",
        "-machine", "pc", "-m", "4096",
        "-vga", "std",
        "-display", "none",
        "-no-reboot",
        "-accel", "tcg",
        "-monitor", f"tcp:127.0.0.1:{PORT_MON},server,nowait",
        "-serial", f"pipe:{PIPE_SHORT}",
    ], stdout=errf, stderr=errf)

    try:
        # Monitor readiness (discard socket so the one-shot server can re-listen).
        if not wait_port(PORT_MON, timeout=40):
            print("FAIL: monitor never came up")
            return 1
        print("[*] monitor up; connecting COM1 named pipe...")
        pipe = open(PIPE_NAME, "rb+", buffering=0)
        rd = threading.Thread(target=pipe_reader, args=(pipe,), daemon=True)
        rd.start()

        print("[*] entering GUI mode (HMP sendkey 'gui')...")
        time.sleep(1.0)
        type_line("gui")

        print("[*] waiting for managed shell / GUI marker...")
        if not read_until(b"NexOS.Forms.Shell initialised", timeout=40.0):
            with SER_LOCK:
                tail = bytes(SER_BUF)[-900:].decode("latin-1", "ignore")
            print("FAIL: managed shell never initialised. serial tail:")
            print(tail)
            return 1
        print("[+] GUI entered. letting desktop register voice controls...")
        time.sleep(3.0)

        # Fire the voice command: matches the 浏览器 / Browser desktop tile.
        for phrase in ("打开浏览器", "open browser"):
            pipe.write(frame(phrase))
            pipe.flush()
            print(f"[*] sent voice frame: {phrase}")
            time.sleep(1.5)

        ok = read_until(b"VOICE:ok", timeout=12.0)
        with SER_LOCK:
            tail = bytes(SER_BUF)[-900:].decode("latin-1", "ignore")
        print("\n--- serial tail ---")
        print(tail[-700:])
        if ok:
            print("\nRESULT: PASS (voice engine fired VOICE:ok)")
            return 0
        print("\nRESULT: FAIL (no VOICE:ok)")
        return 1
    finally:
        try:
            qemu.wait(timeout=5.0)
        except subprocess.TimeoutExpired:
            qemu.terminate()
            qemu.wait(timeout=3.0)
        errf.close()


if __name__ == "__main__":
    sys.exit(main())
