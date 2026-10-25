#!/usr/bin/env python3
"""Live end-to-end test against the REAL kernel running in QEMU.

Requires:  start_ops.ps1 already running (QEMU + bridge).
Sends ops commands over ws://localhost:8765 and prints whatever the
real kernel run_command() echoes back over the serial bridge.
"""
import asyncio, sys
try:
    import websockets
except ImportError:
    print("pip install websockets"); sys.exit(1)

async def main():
    uri = "ws://127.0.0.1:8765"
    try:
        async with websockets.connect(uri) as ws:
            print("[+] connected to bridge")
            for cmd in ["ps", "meminfo", "theme red", "wall sunset", "open calc", "refresh", "ai status"]:
                await ws.send(cmd)
                print(f"\n>>> {cmd}")
                got = ""
                try:
                    while True:
                        msg = await asyncio.wait_for(ws.recv(), timeout=3)
                        got += msg
                        # kernel run_command echoes a line then returns; stop on prompt-ish idle
                        if len(got) > 0 and ("\n" in got) and ("[ops]" not in msg):
                            # read a bit more then break
                            pass
                except asyncio.TimeoutError:
                    pass
                print(got.strip()[:400])
            print("\n[+] done")
    except Exception as e:
        print("connect failed:", e)

asyncio.run(main())
