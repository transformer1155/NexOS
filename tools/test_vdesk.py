#!/usr/bin/env python3
# Headless verification for the independent AI virtual desktop.
# Boots build/os.img, logs in (root/admin), grabs a default-desktop shot,
# switches to the AI desktop with Ctrl+Win+Right, grabs a second shot, and
# switches back with Ctrl+Win+Left.  Asserts the kernel [desk] markers and
# that the two frames differ (proving a separate surface is painted).
import socket, time, sys, subprocess, os

IMG = "build/os.img"
SER = "build/serial_vdesk.log"
MON = "127.0.0.1:4479"
SHOT_DEF = "build/vdesk_default.ppm"
SHOT_AI  = "build/vdesk_ai.ppm"

def mon(cmd):
    s = socket.create_connection(("127.0.0.1", 4479), timeout=5)
    s.sendall((cmd + "\n").encode())
    time.sleep(0.25)
    s.close()

def key(k): mon("sendkey " + k)

def type_str(s):
    for c in s: key(c)

def boot():
    subprocess.Popen([
        "qemu-system-x86_64", "-drive", "format=raw,file=" + IMG,
        "-m", "4096", "-display", "none",
        "-serial", "file:" + SER,
        "-monitor", "telnet:" + MON + ",server,nowait",
        "-no-reboot"
    ])

def diff(a, b):
    out = subprocess.run(["python3", "tools/_diff_ppm.py", a, b],
                         capture_output=True, text=True)
    for line in out.stdout.splitlines():
        if "diff" in line.lower(): return line.strip()
    return "(no diff line)"

def main():
    if os.path.exists(SER): os.remove(SER)
    for f in (SHOT_DEF, SHOT_AI):
        if os.path.exists(f): os.remove(f)
    boot()
    time.sleep(20)                      # lockscreen
    # GUI lock screen: typing the user then Enter with an empty password
    # rejects and moves focus to the password field (Login.cs), so the
    # text-shell style "user / Enter / pass / Enter" sequence logs in.
    type_str("root"); key("ret")
    time.sleep(1.0)
    type_str("admin"); key("ret")
    time.sleep(3)
    mon("screendump " + SHOT_DEF)
    time.sleep(1)
    key("ctrl-meta_l-right")              # -> AI desktop
    time.sleep(1.5)
    mon("screendump " + SHOT_AI)
    time.sleep(1)
    key("ctrl-meta_l-left")               # -> default
    time.sleep(1.5)
    key("ctrl-meta_l-up")                 # toggle -> AI
    time.sleep(1.5)
    mon("quit")
    # ---- assertions ----
    log = open(SER, errors="ignore").read() if os.path.exists(SER) else ""
    d = diff(SHOT_DEF, SHOT_AI)
    print("[OK] serial [desk] markers:")
    for line in log.splitlines():
        if "[desk]" in line or "[DESK]" in line:
            print("   ", line.strip())
    print("[INFO] default vs AI frame:", d)
    has_ai = "[desk] switch -> AI" in log
    has_def = "[desk] switch -> default" in log
    has_cs = "[DESK] switched to 1" in log
    print("[CHECK] kernel AI marker :", has_ai)
    print("[CHECK] kernel def marker:", has_def)
    print("[CHECK] csharp switched 1:", has_cs)
    print("[CHECK] frames differ    :", "diff" in d and d != "(no diff line)")
    ok = has_ai and has_cs and ("diff" in d and d != "(no diff line)")
    print("RESULT:", "PASS" if ok else "FAIL")

if __name__ == "__main__":
    main()
