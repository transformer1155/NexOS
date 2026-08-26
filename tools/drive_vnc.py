#!/usr/bin/env python3
"""Drive the ALREADY-RUNNING VNC QEMU (monitor on :4520) and capture frames.

We connect to the live HMP monitor, inject keystrokes, and screendump at
key moments so we can read what the AI desktop actually renders.
"""
import socket, time, os, struct, zlib, sys

IMG = "build/os_textboot.img"
SHOT_DIR = "build/ai_shots"
os.makedirs(SHOT_DIR, exist_ok=True)
MON = ("127.0.0.1", 4520)
GOAL = "hello"

def connect():
    s = socket.create_connection(MON, timeout=5)
    s.settimeout(3.0)
    # drain banner
    time.sleep(0.5)
    try:
        s.recv(4096)
    except Exception:
        pass
    return s

def cmd(s, c):
    s.sendall((c + "\n").encode())
    time.sleep(0.35)
    out = b""
    s.settimeout(2.0)
    try:
        while True:
            d = s.recv(4096)
            if not d:
                break
            out += d
            if out.rstrip().endswith(b"(qemu)"):
                break
    except Exception:
        pass
    return out.decode("latin-1", "ignore")

def send_key(s, key, delay=0.14):
    cmd(s, "sendkey %s" % key)
    time.sleep(delay)

def type_line(s, text, delay=0.22):
    for ch in text:
        key = ("shift-%s" % ch.lower()) if ('A' <= ch <= 'Z') else ch
        send_key(s, key, delay)
    send_key(s, "ret", 0.5)

def read_log():
    try:
        return open("build/serial_vnc.log", "r", errors="ignore").read()
    except Exception:
        return ""

def shot(s, name):
    ppm = os.path.join(SHOT_DIR, name + ".ppm")
    png = os.path.join(SHOT_DIR, name + ".png")
    cmd(s, "screendump %s" % ppm)
    # wait for file to be fully written
    sz0 = -1
    for _ in range(20):
        try:
            sz = os.path.getsize(ppm)
        except Exception:
            sz = -1
        if sz == sz0 and sz > 0:
            break
        sz0 = sz
        time.sleep(0.1)
    if ppm_to_png(ppm, png):
        print("  [shot] %s -> %s" % (name, png))
        return png
    print("  [shot] %s FAILED" % name)
    return None

def ppm_to_png(ppm, png):
    try:
        data = open(ppm, "rb").read()
        if data[:2] != b"P6":
            return False
        idx = 2
        fields = []
        while len(fields) < 3:
            while idx < len(data) and data[idx] in b" \t\n\r":
                idx += 1
            st = idx
            while idx < len(data) and data[idx] not in b" \t\n\r":
                idx += 1
            fields.append(int(data[st:idx]))
        w, h, mx = fields
        idx += 1
        raw = data[idx:]
        rows = b"".join(raw[r*w*3:r*w*3+w*3] for r in range(h))

        def chunk(typ, body):
            return struct.pack(">I", len(body)) + typ + body + struct.pack(">I", zlib.crc32(typ+body) & 0xffffffff)

        png_out = b"\x89PNG\r\n\x1a\n"
        png_out += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
        png_out += chunk(b"IDAT", zlib.compress(rows, 6))
        png_out += chunk(b"IEND", b"")
        open(png, "wb").write(png_out)
        return True
    except Exception as e:
        print("  ppm_to_png err:", e)
        return False

def main():
    s = connect()
    print("[DRIVE] connected to HMP", MON)

    # ---- login (with warmup to avoid first-char drop) ----
    print("[LOGIN] root / admin")
    time.sleep(0.5)
    send_key(s, "shift-r", 0.2)  # warmup char, harmless
    send_key(s, "backspace", 0.2)
    type_line(s, "root", 0.25)
    time.sleep(0.8)
    type_line(s, "admin", 0.25)
    time.sleep(2.5)
    if "Welcome" in read_log() or "Shell ready" in read_log():
        print("  [ok] login confirmed via serial")
    else:
        print("  [warn] login not confirmed; serial tail:")
        print("    " + read_log()[-200:].replace("\n", "\n    "))

    print("[GUI] gui")
    type_line(s, "gui", 0.2)
    time.sleep(10.0)
    shot(s, "v_desktop0")

    print("[GUI] Ctrl+Right -> AI desktop")
    send_key(s, "ctrl-right", 2.0)
    shot(s, "v_ai_idle")

    print("[AI] type '%s'" % GOAL)
    for ch in GOAL:
        send_key(s, ch, 0.2)
    time.sleep(1.8)
    shot(s, "v_goal_typed")

    print("[AI] Enter -> send")
    send_key(s, "ret", 0.1)
    # capture progression
    for dt, name in [(0.3, "v_think_a"), (0.7, "v_think_b"), (1.2, "v_tw1"),
                     (3.0, "v_tw2"), (5.0, "v_tw3"), (8.0, "v_done")]:
        time.sleep(dt)
        shot(s, name)
    time.sleep(2.0)
    shot(s, "v_stable")

    print("[DONE] captured frames in", SHOT_DIR)
    s.close()

if __name__ == "__main__":
    main()
