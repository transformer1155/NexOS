#!/usr/bin/env python3
"""Headless QEMU test for the Win32 subsystem using a TCP HMP monitor."""
import os, sys, socket, time, subprocess, struct, zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)

IMG = sys.argv[1] if len(sys.argv) > 1 else "build/os.img"
WORK = "build/w32test.img"
MON_PORT = 4444
MON_TIMEOUT = 30.0


def type_line(sock, s, delay=0.20):
    """Send monitor sendkey commands for a string, then Enter."""
    keymap = {
        ' ': 'spc', '.': 'dot', '/': 'slash', '\\': 'backslash',
        '-': 'minus', '_': 'shift-minus',
    }
    for ch in s:
        key = keymap.get(ch)
        if key is None:
            if 'A' <= ch <= 'Z':
                key = f"shift-{ch.lower()}"
            else:
                key = ch
        sock.sendall(f"sendkey {key}\n".encode())
        time.sleep(0.06)
    sock.sendall(b"sendkey ret\n")
    time.sleep(0.15)
    time.sleep(delay)


def mon_read_until(sock, marker=b"(qemu)", timeout=2.0):
    sock.settimeout(timeout)
    buf = b""
    try:
        while marker not in buf:
            data = sock.recv(4096)
            if not data:
                break
            buf += data
    except socket.timeout:
        pass
    return buf


def wait_for_socket(port, timeout):
    end = time.time() + timeout
    while time.time() < end:
        try:
            return socket.create_connection(("127.0.0.1", port), timeout=0.5)
        except OSError:
            time.sleep(0.2)
    raise RuntimeError(f"QEMU monitor did not become ready on port {port}")


def make_png(ppm_path, png_path, scale=2):
    with open(ppm_path, "rb") as f:
        d = f.read()
    def tok(i):
        while d[i:i+1].isspace(): i += 1
        j = i
        while not d[j:j+1].isspace(): j += 1
        return d[i:j], j
    _, i = tok(0); w, i = tok(i); h, i = tok(i); _, i = tok(i); i += 1
    w, h = int(w), int(h)
    W, H = w // scale, h // scale
    px = d[i:]
    raw = bytearray()
    for y in range(0, H * scale, scale):
        raw.append(0)
        for x in range(0, W * scale, scale):
            o = (y * w + x) * 3
            raw += px[o:o+3]
    def chunk(t, data):
        c = t + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c) & 0xffffffff)
    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 6))
    png += chunk(b"IEND", b"")
    with open(png_path, "wb") as f:
        f.write(png)
    return W, H


def main():
    if not os.path.exists(IMG):
        print(f"missing {IMG} - run 'make build/os.img' first")
        return 1
    subprocess.run(["cp", IMG, WORK], check=True)

    qemu = subprocess.Popen(
        [
            "qemu-system-x86_64",
            "-drive", f"format=raw,file={WORK}",
            "-m", "128M",
            "-vga", "std",
            "-display", "none",
            "-no-reboot",
            "-monitor", f"tcp:127.0.0.1:{MON_PORT},server,nowait",
            "-chardev", "file,id=ser,path=build/serial.log",
            "-serial", "chardev:ser",
        ],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )

    try:
        print("==> Waiting for QEMU monitor...")
        mon = wait_for_socket(MON_PORT, MON_TIMEOUT)
        mon_read_until(mon)  # consume banner

        print("==> Waiting for login prompt...")
        time.sleep(8.0)

        # Log in as root/admin
        type_line(mon, "root", 0.5)
        type_line(mon, "admin", 1.0)

        # Session 1: text-mode commands
        type_line(mon, "lsfs", 0.8)
        type_line(mon, "winver", 0.8)
        type_line(mon, "winenv", 0.8)
        mon.sendall(b"screendump build/w32_s1.ppm\n")
        time.sleep(0.5)

        type_line(mon, "clear", 0.4)
        type_line(mon, "reg query HKLM\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", 1.2)
        mon.sendall(b"screendump build/w32_s2.ppm\n")
        time.sleep(0.5)

        type_line(mon, "clear", 0.4)
        type_line(mon, "winapp /i hello32.exe", 1.5)
        mon.sendall(b"screendump build/w32_s3.ppm\n")
        time.sleep(0.5)

        # Session 2: launch GUI
        type_line(mon, "clear", 0.4)
        type_line(mon, "winapp hello32.exe", 3.0)
        time.sleep(8.0)
        mon.sendall(b"screendump build/w32_gui.ppm\n")
        time.sleep(0.5)

        mon.sendall(b"quit\n")
        time.sleep(0.5)
    finally:
        try:
            qemu.wait(timeout=5.0)
        except subprocess.TimeoutExpired:
            qemu.terminate()
            qemu.wait(timeout=3.0)

    # Render PNGs
    print("==> Captured frames:")
    for name in ["w32_s1", "w32_s2", "w32_s3", "w32_gui"]:
        ppm = f"build/{name}.ppm"
        png = f"build/{name}.png"
        if os.path.exists(ppm):
            make_png(ppm, png)
            print(f"  {png}")

    print("\n==> Serial log tail:")
    if os.path.exists("build/serial.log"):
        with open("build/serial.log", "rb") as f:
            data = f.read().decode("latin-1", "ignore")
        print("\n".join(data.splitlines()[-30:]))

    return 0


if __name__ == "__main__":
    sys.exit(main())
