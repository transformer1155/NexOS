#!/usr/bin/env python3
"""
Headless end-to-end test for the NexOS ops-panel pipeline.

Stands in for the real kernel's serial listener: opens tcp:127.0.0.1:4321,
reads command lines, runs a minimal run_command() that mirrors the ops
branches added to kernel.cpp, and writes the output back over the socket
(exactly how serial_poll_input -> run_command -> serial_puts works).

Then drives the real nexos_bridge.py over WebSocket with the same commands
the browser would send, and asserts the echoed output is correct.
"""
import asyncio
import socket
import sys
import time

SERIAL_PORT = 4321
WS_PORT = 8765

# ---- minimal mock of kernel run_command ops branches ----
def run_command(line):
    parts = line.strip().split(maxsplit=1)
    cmd = parts[0].lower() if parts else ""
    arg = parts[1] if len(parts) > 1 else ""
    out = []
    if cmd == "theme":
        out.append(f"theme -> {arg}")
    elif cmd in ("wall", "wallpaper"):
        out.append(f"wallpaper -> {arg}")
    elif cmd == "refresh":
        out.append("desktop refreshed")
    elif cmd == "open":
        out.append(f"launching app: {arg}")
    elif cmd == "calc":
        out.append("launching app: calculator")
    elif cmd in ("arrange-icons", "arrange"):
        out.append("icons rearranged")
    elif cmd == "view":
        out.append(f"icon view -> {arg}")
    elif cmd == "ai":
        out.append(f"AI> thinking about: {arg}")
    elif cmd == "ps":
        out.append("PID  NAME         CPU\n4    System       2%\n300  browser      8%")
    elif cmd == "meminfo":
        out.append("total 4096 MB  free 3120 MB")
    elif cmd == "help":
        out.append("commands: open theme wall refresh ai ps meminfo ...")
    else:
        out.append(f"[nexos] {line}")
    return "\n".join(out) + "\n"

def serial_server():
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", SERIAL_PORT))
    srv.listen(1)
    srv.settimeout(0.5)
    print("[mock-kernel] serial tcp listening on", SERIAL_PORT)
    while True:
        try:
            conn, _ = srv.accept()
        except socket.timeout:
            continue
        print("[mock-kernel] bridge connected")
        buf = b""
        with conn:
            while True:
                try:
                    data = conn.recv(1024)
                except Exception:
                    break
                if not data:
                    break
                buf += data
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    cmd = line.decode("utf-8", "replace")
                    resp = run_command(cmd)
                    conn.sendall(("[ops] " + cmd + "\n").encode())
                    conn.sendall(resp.encode())

async def main():
    import websockets
    # start mock kernel
    import threading
    t = threading.Thread(target=serial_server, daemon=True)
    t.start()
    time.sleep(0.5)

    # connect through the REAL bridge
    uri = f"ws://127.0.0.1:{WS_PORT}"
    try:
        async with websockets.connect(uri) as ws:
            cases = [
                ("open calc", "launching app: calc"),
                ("theme red", "theme -> red"),
                ("wall sunset", "wallpaper -> sunset"),
                ("refresh", "desktop refreshed"),
                ("ai hello world", "AI> thinking about: hello world"),
                ("ps", "browser"),
                ("meminfo", "free 3120 MB"),
            ]
            passed = 0
            for cmd, expect in cases:
                await ws.send(cmd)
                # read until we see the expected substring or timeout
                got = ""
                try:
                    for _ in range(20):
                        msg = await asyncio.wait_for(ws.recv(), timeout=2)
                        got += msg
                        if expect in msg:
                            break
                except asyncio.TimeoutError:
                    pass
                ok = expect in got
                print(f"  [{'PASS' if ok else 'FAIL'}] {cmd!r}  ->  {got.strip()[:60]!r}")
                passed += 1 if ok else 0
            print(f"\n{mypassed(passed, len(cases))}")
    except Exception as e:
        print("WS connect failed:", e)

def mypassed(p, n):
    return f"{p}/{n} cases passed" + ("  ALL GOOD" if p == n else "  SOME FAILED")

if __name__ == "__main__":
    asyncio.run(main())
