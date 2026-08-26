#!/usr/bin/env python3
"""Headless end-to-end smoke test for the NexOS voice engine on the UEFI path.

Boots build/os_uefi_voice.img under QEMU (TCG, q35, ramfb, OVMF) -- the path
that actually reaches the GUI under TCG (the BIOS/pc path triple-faults in
the 64-bit kernel's build_idt under TCG).  The UEFI image auto-enters the
Win11 GUI, so no `gui` keystroke is required.

Serial topology (deadlock-free by construction):
  * COM1 (0x3F8) -> file  : guest console output only.  The host reads the
    file to detect "NexOS.Forms.Shell initialised" and to grab the tail.
  * COM2 (0x2F8) -> TCP   : VOICE INPUT.  QEMU exposes COM2 as a TCP server;
    the host connects and uses socket.send (buffered, never blocks on slow
    guest consumption).  A background recv thread drains the guest->host
    direction so OVMF's debug writes to COM2 can never stall the boot.

TCG/UEFI boot is occasionally flaky.  We retry the whole QEMU lifecycle up to
MAX_ATTEMPTS times; the first attempt that reaches the shell and fires the
voice engine wins.

Protocol (see kernel64.cpp serial_voice_poll):
    byte 0   = 0x02  (STX)
    byte 1   = length (1..127)
    bytes 2+ = UTF-8 phrase bytes
"""
import os, socket, time, subprocess, sys, shutil, threading

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)

IMG = "build/os_uefi_voice.img"
WORK = "build/uefi_voice_test.img"
SER_FILE = "build/serial_voice_uefi.txt"
QEMU = r"D:\qemu\qemu-system-x86_64.exe"
OVMF_CODE = r"D:\qemu\share\edk2-x86_64-code.fd"
OVMF_VARS = os.path.join(ROOT, "build", "ovmf_vars_test.fd")
PORT_MON = 4463
VOICE_PORT = 4464
MAX_ATTEMPTS = 3
MARKER_TIMEOUT = 90.0          # per-attempt: wait for managed shell
VOICE_TIMEOUT = 12.0           # per-attempt: wait for VOICE:ok after frames
GLOBAL_HARD_TIMEOUT = 240.0    # absolute backstop

SER_BUF = bytearray()
SER_LOCK = threading.Lock()


def log(msg):
    print(msg, flush=True)


def mon_send(cmd, retries=6, port=PORT_MON):
    for attempt in range(retries):
        try:
            s = socket.create_connection(("127.0.0.1", port), timeout=5)
            s.sendall((cmd + "\n").encode())
            time.sleep(0.3)
            s.close()
            return True
        except Exception as e:
            if attempt + 1 >= retries:
                log(f"[MON] send failed: {e}")
                return False
            time.sleep(0.5)
    return False


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


def read_serial_file():
    """Append any new console bytes from the COM1 file into SER_BUF."""
    try:
        with open(SER_FILE, "rb") as f:
            f.seek(len(SER_BUF))
            chunk = f.read(65536)
            if chunk:
                with SER_LOCK:
                    SER_BUF.extend(chunk)
    except Exception:
        pass


def read_until(marker, timeout=30.0):
    end = time.time() + timeout
    while time.time() < end:
        read_serial_file()
        with SER_LOCK:
            if marker in bytes(SER_BUF):
                return True
        time.sleep(0.2)
    return False


def frame(phrase):
    b = phrase.encode("utf-8")
    return b"\x02" + bytes([len(b) & 0x7F]) + b


def free_port():
    """Return a currently-unused TCP port on localhost (best-effort)."""
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def open_voice_sock(timeout=15.0, port=VOICE_PORT):
    end = time.time() + timeout
    while time.time() < end:
        try:
            return socket.create_connection(("127.0.0.1", port), timeout=5)
        except Exception:
            time.sleep(0.4)
    return None


def send_frame_bounded(sock, data, timeout=5.0):
    """Return ("ok", None) | ("err", reason) | ("timeout", None)."""
    result = {"ok": False, "err": None}

    def worker():
        try:
            sock.sendall(data)
            result["ok"] = True
        except Exception as e:
            result["err"] = repr(e)
            result["ok"] = False
    t = threading.Thread(target=worker, daemon=True)
    t.start()
    t.join(timeout)
    if t.is_alive():
        return ("timeout", None)
    if result["ok"]:
        return ("ok", None)
    return ("err", result["err"])


def run_once(attempt):
    """Boot one QEMU instance, reach the GUI, inject voice frames.

    Returns "voice_ok", "shell_only", or None (boot failed/hung).
    """
    log(f"\n===== ATTEMPT {attempt}/{MAX_ATTEMPTS} =====")
    shutil.copy(IMG, WORK)
    open(SER_FILE, "w").close()
    with SER_LOCK:
        SER_BUF.clear()
    errf = open("build/qemu_voice_uefi.err", "wb")

    mon_port = free_port()
    voice_port = free_port()
    while voice_port == mon_port:
        voice_port = free_port()
    qemu = subprocess.Popen([
        QEMU,
        "-machine", "q35", "-m", "1536",
        "-accel", "tcg",
        "-vga", "none",
        "-device", "ramfb,id=rfb",
        "-display", "none",
        "-drive", f"if=pflash,format=raw,readonly=on,file={OVMF_CODE}",
        "-drive", f"if=pflash,format=raw,file={OVMF_VARS}",
        "-drive", f"file={WORK},format=raw,if=ide",
        "-serial", f"file:{SER_FILE}",                       # COM1: console
        "-serial", f"tcp:127.0.0.1:{voice_port},server,nowait",  # COM2: voice
        "-monitor", f"tcp:127.0.0.1:{mon_port},server,nowait",
    ], stdout=errf, stderr=errf)

    result = None
    try:
        if not wait_port(mon_port, timeout=40):
            log("FAIL: monitor never came up")
            return None
        log("[*] monitor up; connecting COM2 voice TCP...")
        sock = open_voice_sock(15.0, voice_port)
        if sock is None:
            log("FAIL: COM2 voice TCP never became available")
            return None

        # Drain guest->host so OVMF debug writes to COM2 never stall QEMU.
        def _drain(s):
            try:
                while True:
                    if not s.recv(4096):
                        break
            except Exception:
                pass
        threading.Thread(target=_drain, args=(sock,), daemon=True).start()

        log("[*] waiting for managed shell / GUI marker (COM1 file)...")
        if not read_until(b"NexOS.Forms.Shell initialised", timeout=MARKER_TIMEOUT):
            read_serial_file()
            with SER_LOCK:
                tail = bytes(SER_BUF)[-1000:].decode("latin-1", "ignore")
            log("ATTEMPT FAILED: managed shell never initialised. serial tail:")
            log(tail)
            return None

        log("[+] GUI + managed shell up. letting desktop register voice controls...")
        time.sleep(3.0)

        for phrase in ("打开浏览器", "open browser"):
            ok, err = send_frame_bounded(sock, frame(phrase), timeout=5.0)
            log(f"[*] sent voice frame '{phrase}': status={ok} err={err}")
            time.sleep(1.5)

        if read_until(b"VOICE:ok", timeout=VOICE_TIMEOUT):
            result = "voice_ok"
        else:
            result = "shell_only"

        read_serial_file()
        with SER_LOCK:
            tail = bytes(SER_BUF)[-700:].decode("latin-1", "ignore")
            full = bytes(SER_BUF).decode("latin-1", "ignore")
        log("--- serial tail ---")
        log(tail)
        for l in full.splitlines():
            if "[LSR2]" in l or "[VSTX]" in l or "[VRX]" in l or "[VSAY]" in l:
                log(f"[diag] {l}")
        return result
    finally:
        try:
            sock.close()
        except Exception:
            pass
        mon_send("quit", port=mon_port)
        try:
            qemu.wait(timeout=6.0)
        except subprocess.TimeoutExpired:
            qemu.terminate()
            try:
                qemu.wait(timeout=3.0)
            except subprocess.TimeoutExpired:
                try:
                    qemu.kill()
                except Exception:
                    pass
        errf.close()


def main():
    if not os.path.exists(IMG):
        log("ERROR: build/os_uefi_voice.img missing; rebuild kernel64 + dd first")
        return 2

    def watchdog():
        time.sleep(GLOBAL_HARD_TIMEOUT)
        log("[WATCHDOG] global hard timeout hit; force-killing process")
        os._exit(3)
    threading.Thread(target=watchdog, daemon=True).start()

    for attempt in range(1, MAX_ATTEMPTS + 1):
        r = run_once(attempt)
        if r == "voice_ok":
            log("\nRESULT: PASS (voice engine fired VOICE:ok)")
            return 0
        if r == "shell_only":
            log("\nRESULT: FAIL (shell up but no VOICE:ok)")
            return 1
        # None -> boot hung/failed; loop retries a fresh QEMU instance
        log(f"[*] attempt {attempt} inconclusive, retrying...")
        time.sleep(2.0)

    log("\nRESULT: FAIL (exhausted retries; boot never reached GUI reliably)")
    return 1


if __name__ == "__main__":
    sys.exit(main())
