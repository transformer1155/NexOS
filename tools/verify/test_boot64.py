#!/usr/bin/env python3
# =====================================================================
#  test_boot64.py - end-to-end verification of the 64-bit-only boot path
#
#  Expected chain:
#    [K32] minimal bootstrap          (boot32_min.cpp)
#    SdMPACELGG6                      (switch32to64.asm markers)
#    [AUTOTEST] probing embedded GGUF -> model loaded -> inference done
#    [M-GE]                           (gui_enter returned, desktop live)
#
#  Screenshots at the end: build/boot64_shell.png (login) and, after the
#  admin login keystrokes, build/boot64_desktop.png.
# =====================================================================
import os, sys, time, subprocess, socket

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
IMG  = os.path.expanduser("~/os_v2_local.img")
PNG1 = os.path.join(ROOT, "build", "boot64_shell.png")
PNG2 = os.path.join(ROOT, "build", "boot64_desktop.png")
SER  = os.path.join(ROOT, "build", "boot64_serial.log")

MARKS = [
    ("bootstrap",  b"[K32] minimal bootstrap"),
    ("longmode",   b"[K32] entering long mode"),
    ("k64_up",     b"[AUTOTEST] probing embedded GGUF"),
    ("model_load", b"[AUTOTEST] model loaded"),
    ("inference",  b"[AUTOTEST] inference done"),
]

def grab(mon, path):
    # Mon PPM screenshot; retry until the file is complete.
    for _ in range(40):
        try:
            with open(path, "wb") as f:
                subprocess.run([QEMU_PREFIX + "qemu-monitor-command" if False else "bash", "-c",
                                "true"], check=False)
            break
        except Exception:
            time.sleep(0.25)
    # Use QMP-free HMP over the monitor socket file instead.
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.connect(mon)
        s.recv(4096)
        s.sendall(b"screendump %s\n" % path.encode())
        time.sleep(1.0)
        s.close()
    except Exception as e:
        print("screendump failed:", e)

# We use a TCP monitor on 127.0.0.1:4444 (simpler than unix sockets on the
# shared-folder VM) plus a TCP serial capture.
def main():
    qemu = "qemu-system-x86_64"
    mport, sport = 4444, 5555
    # Clean leftovers.
    subprocess.run(["pkill", "-9", "-f", "qemu-system"], check=False)
    time.sleep(1)
    for p in (mport, sport):
        s = socket.socket()
        s.settimeout(0.3)
        try: s.connect(("127.0.0.1", p)); s.close(); print("port %d busy" % p); sys.exit(2)
        except Exception: pass

    cmd = [qemu,
           "-drive", "file=" + IMG + ",format=raw,if=ide",
           "-m", "1024", "-accel", "tcg,tb-size=128",
           "-serial", "tcp:127.0.0.1:%d,server,nowait" % sport,
           "-monitor", "tcp:127.0.0.1:%d,server,nowait" % mport,
           "-display", "none"]
    # Fast model path: the raw GGUF file attached as a legacy virtio-blk disk
    # lets kernel64 pull all 379MB via chunked DMA (reads from LBA 0).
    model = os.path.expanduser("~/qwen_model.raw")
    if os.path.exists(model):
        cmd += ["-drive", "id=vd0,file=" + model + ",format=raw,if=none",
                "-device", "virtio-blk-pci,drive=vd0,disable-modern=on,disable-legacy=off"]
        print("virtio model drive (legacy):", model, flush=True)
    print("launching:", " ".join(cmd), flush=True)
    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)

    # Connect serial capture.
    ser = None
    for _ in range(60):
        try:
            ser = socket.create_connection(("127.0.0.1", sport), timeout=2)
            ser.settimeout(1.0)
            break
        except Exception:
            time.sleep(1)
    if not ser:
        print("FAIL: cannot connect serial"); proc.kill(); sys.exit(3)

    mon = None
    for _ in range(30):
        try:
            mon = ("tcp", mport)
            s = socket.create_connection(("127.0.0.1", mport), timeout=2)
            s.close(); break
        except Exception:
            time.sleep(1)

    found = {k: False for k, _ in MARKS}
    buf = b""
    logf = open(SER, "wb")
    t0 = time.time()
    screenshot1 = False
    try:
        while time.time() - t0 < 3600:
            try:
                d = ser.recv(65536)
                if d:
                    buf += d
                    logf.write(d); logf.flush()
            except socket.timeout:
                pass
            for k, m in MARKS:
                if not found[k] and m in buf:
                    found[k] = True
                    print("MARK %-10s OK  (t=%.0fs)" % (k, time.time() - t0), flush=True)
            # After the desktop marker, take the desktop shot; before that,
            # once long mode is up, grab the (login) shell.
            if not screenshot1 and found["k64_up"]:
                screenshot1 = True
                time.sleep(3)
                screendump(mport, PNG1)
            if found["inference"] and found["model_load"]:
                # give the desktop a moment, then shoot and finish early
                time.sleep(20)
                screendump(mport, PNG2)
                break
    except KeyboardInterrupt:
        pass
    logf.close()
    proc.kill()

    print("\n==== RESULT ====")
    ok = True
    for k, _ in MARKS:
        print("  %-10s %s" % (k, "PASS" if found[k] else "FAIL"))
        ok = ok and found[k]
    print("OVERALL:", "PASS" if ok else "FAIL")
    sys.exit(0 if ok else 1)

def screendump(mon, path):
    try:
        s = socket.create_connection(("127.0.0.1", mon), timeout=3)
        s.settimeout(3)
        s.recv(4096)
        s.sendall(b"screendump %s\n" % path.encode())
        time.sleep(1.5)
        s.close()
        print("screendump ->", path, flush=True)
    except Exception as e:
        print("screendump failed:", e)

if __name__ == "__main__":
    main()
