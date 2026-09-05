#!/usr/bin/env python3
"""Headless end-to-end test: boot NexOS under QEMU (serial on TCP 4321),
connect, send 'about' and a few commands, verify kernel echoes output."""
import socket, subprocess, sys, time, os, signal, threading

QEMU = "qemu-system-x86_64"
IMG = "/mnt/d/MyOS/bootloader/build/os_v2.img"
SERIAL_PORT = 4321
SERIAL_HOST = "127.0.0.1"

def boot_qemu():
    cmd = [
        QEMU, "-drive", f"file={IMG},format=raw,if=ide",
        "-serial", f"tcp:{SERIAL_HOST}:{SERIAL_PORT},server,nowait",
        "-display", "none", "-m", "1024", "-accel", "tcg",
    ]
    print("[test] launching QEMU headless (serial tcp %d)..." % SERIAL_PORT)
    return subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

def recv_all(s, wait=2.0):
    s.settimeout(wait)
    data = b""
    try:
        while True:
            chunk = s.recv(4096)
            if not chunk:
                break
            data += chunk
    except socket.timeout:
        pass
    return data

def main():
    q = boot_qemu()
    time.sleep(3)
    try:
        s = socket.create_connection((SERIAL_HOST, SERIAL_PORT), timeout=8)
        time.sleep(8)  # let kernel fully boot + enter GUI (serial polling on)
        # drain initial boot output
        _ = recv_all(s, 1.0)
        results = {}
        for cmd in ["login nexos nexos", "whoami", "about", "help"]:
            s.sendall((cmd + "\n").encode())
            time.sleep(1.5)
            out = recv_all(s, 1.5).decode("utf-8", "replace")
            results[cmd] = out
            print(f"\n==== OUT for '{cmd}' ====")
            print(out[-600:] if len(out) > 600 else out)
        # summarize
        print("\n==== SUMMARY ====")
        joined = "\n".join(results.values())
        any_out = any(len(v.strip()) > 0 for v in results.values())
        logged_in = ("Logged in as nexos" in joined)
        print("kernel responds with output:", any_out)
        print("login authenticated          :", logged_in)
        s.close()
    finally:
        q.send_signal(signal.SIGTERM)
        try:
            q.wait(timeout=5)
        except Exception:
            q.kill()

if __name__ == "__main__":
    main()
