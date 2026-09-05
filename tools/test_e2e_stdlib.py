#!/usr/bin/env python3
"""End-to-end test of the NexOS remote console flow using a pure-stdlib
WebSocket client (no third-party websockets module needed).

Boots NexOS headless with serial on a chardev socket (4321), starts
nexos_bridge.py (WS 8765), then connects as the web frontend would and
exercises the full flow:
  login nexos nexos  -> authenticate against kernel account
  whoami             -> nexos
  about              -> system info
  users / ls         -> command forwarded to backend
"""
import socket, subprocess, sys, time, os, base64, struct, hashlib, threading

HERE = os.path.dirname(os.path.abspath(__file__))
# Choose correct paths depending on where this runs (WSL vs native Windows)
import sys
if sys.platform.startswith("win"):
    IMG = r"D:\MyOS\bootloader\build\os_v2.img"
    QEMU = r"D:\qemu\qemu-system-x86_64.exe"
else:
    IMG = "/mnt/d/MyOS/bootloader/build/os_v2.img"
    QEMU = "/mnt/d/qemu/qemu-system-x86_64.exe"
WS_PORT = 8765
SERIAL_PORT = 4321
HOST = "127.0.0.1"


def ws_connect(host, port):
    s = socket.create_connection((host, port), timeout=8)
    key = base64.b64encode(os.urandom(16)).decode()
    s.sendall(
        f"GET / HTTP/1.1\r\nHost: {host}:{port}\r\nUpgrade: websocket\r\n"
        f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\n"
        f"Sec-WebSocket-Version: 13\r\n\r\n".encode()
    )
    # read handshake response
    resp = b""
    while b"\r\n\r\n" not in resp:
        resp += s.recv(1)
    if b"101" not in resp:
        raise RuntimeError("WS handshake failed: " + resp.decode(errors="replace"))
    return s


def ws_send(s, text):
    data = text.encode("utf-8")
    mask = os.urandom(4)
    mask_key = struct.unpack(">I", mask)[0]
    masked = bytes(b ^ mask[i % 4] for i, b in enumerate(data))
    length = len(data)
    if length < 126:
        header = struct.pack(">BB", 0x81, 0x80 | length)
    else:
        header = struct.pack(">BBH", 0x81, 0x80 | 126, length)
    s.sendall(header + mask + masked)


def ws_recv(s, timeout=3):
    s.settimeout(timeout)
    out = b""
    try:
        while True:
            hdr = s.recv(2)
            if len(hdr) < 2:
                break
            opcode = hdr[0] & 0x0F
            plen = hdr[1] & 0x7F
            if plen == 126:
                plen = struct.unpack(">H", s.recv(2))[0]
            elif plen == 127:
                plen = struct.unpack(">Q", s.recv(8))[0]
            payload = b""
            while len(payload) < plen:
                chunk = s.recv(plen - len(payload))
                if not chunk:
                    break
                payload += chunk
            if opcode == 0x8:  # close
                break
            if payload:
                out += payload
    except socket.timeout:
        pass
    return out.decode("utf-8", "replace")


def wait_port(port, timeout=10):
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            with socket.create_connection((HOST, port), timeout=1):
                return True
        except OSError:
            time.sleep(0.3)
    return False


def main():
    qemu = subprocess.Popen(
        [QEMU, "-drive", f"file={IMG},format=raw", "-chardev",
         f"socket,id=ser0,server=on,wait=off,host={HOST},port={SERIAL_PORT}",
         "-serial", "chardev:ser0", "-display", "none", "-vga", "std",
         "-machine", "pc,mem-merge=off", "-m", "512", "-accel", "tcg",
         "-no-reboot"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    bridge = subprocess.Popen(
        [sys.executable, os.path.join(HERE, "nexos_bridge.py")],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        assert wait_port(SERIAL_PORT, 12), "QEMU serial not up"
        assert wait_port(WS_PORT, 12), "bridge WS not up"
        time.sleep(20)  # let kernel fully boot + enter GUI (serial stable)
        ws = ws_connect(HOST, WS_PORT)
        results = {}
        ws_send(ws, "login nexos nexos")
        time.sleep(3)
        results["login nexos nexos"] = ws_recv(ws, 3)
        for cmd in ["whoami", "about", "users", "ls"]:
            ws_send(ws, cmd)
            time.sleep(3)
            results[cmd] = ws_recv(ws, 3)
            print(f"\n==== FRONTEND RECEIVED for '{cmd}' ====")
            print(results[cmd][-500:] if len(results[cmd]) > 500 else results[cmd])
        joined = "\n".join(results.values())
        print("\n==== SUMMARY ====")
        checks = {
            "login authenticated": "Logged in as nexos" in joined,
            "whoami -> nexos": "nexos" in results.get("whoami", ""),
            "about echoed": "NexOS" in results.get("about", ""),
            "users/ls forwarded (any IN)": len(results.get("users", "").strip()) > 0 or len(results.get("ls", "").strip()) > 0,
        }
        for k, v in checks.items():
            print(f"  [{'PASS' if v else 'FAIL'}] {k}")
        ws.close()
    finally:
        for p in (qemu, bridge):
            try: p.terminate()
            except Exception: pass
        time.sleep(1)
        for p in (qemu, bridge):
            try: p.kill()
            except Exception: pass


if __name__ == "__main__":
    main()
