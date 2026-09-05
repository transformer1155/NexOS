#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Live end-to-end test of the AI virtual desktop (real boot + real screenshots).

Boots build/os_textboot.img in a *headless* QEMU, drives the keyboard through
the HMP monitor, and captures the framebuffer with `screendump` at every
meaningful phase so a human can see exactly what appeared on screen:

    boot -> login -> gui (Win11 desktop 0) -> Ctrl+Right (AI desktop)
    -> type a goal -> Enter -> "思考中…" -> typewriter reply reveal

The AI engine runs for real: in a VM it selects the built-in Markov engine
(trained on a 27-sentence corpus) and `agent run` returns genuinely generated
character-level text.  The serial console is logged so we can prove the CLR
fault (opcode 0x12) that previously blanked the screen is gone.

Screenshots are written as PPM (QEMU native) then converted to PNG with PIL.
"""
import os
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)

IMG = "build/os_textboot.img"
LOG = "build/serial_ai_live.log"
ERR = "build/qemu_ai_live.err"
PORT = 4517
SHOT_DIR = "build/ai_shots"

BOOT_MARKERS = ("Shell ready", "[K1] kmain entered", "PS ")
GOAL = "hello"


def wait_sock(port, timeout=40.0):
    end = time.time() + timeout
    while time.time() < end:
        try:
            return socket.create_connection(("127.0.0.1", port), timeout=0.5)
        except OSError:
            time.sleep(0.2)
    raise RuntimeError("QEMU monitor did not come up on port %d" % port)


def read_log():
    try:
        with open(LOG, "rb") as f:
            return f.read().decode("latin-1", "ignore")
    except OSError:
        return ""


def wait_for(markers, timeout, label):
    end = time.time() + timeout
    while time.time() < end:
        if any(m in read_log() for m in markers):
            return True
        time.sleep(0.4)
    return False


def send_key(mon, key, delay=0.12):
    mon.sendall(("sendkey %s\n" % key).encode())
    time.sleep(delay)


def type_line(mon, s, delay=0.22):
    for ch in s:
        key = ("shift-%s" % ch.lower()) if 'A' <= ch <= 'Z' else ch
        send_key(mon, key, delay)
    send_key(mon, "ret", 0.6)


def type_word(mon, s, delay=0.22):
    """Type without a trailing Enter (for the chat input box)."""
    for ch in s:
        key = ("shift-%s" % ch.lower()) if 'A' <= ch <= 'Z' else ch
        send_key(mon, key, delay)


def login_root(mon, retries=4):
    """Log in as root/admin.  The first keystrokes after boot are sometimes
    dropped by the PS/2 keyboard controller, so we (a) settle a few seconds,
    (b) send a harmless leading Enter to 'warm up' the controller, and
    (c) retry the whole sequence if 'Login incorrect' appears."""
    for attempt in range(retries):
        # warm-up: an empty username just re-prompts, harmless
        send_key(mon, "ret", 0.4)
        type_line(mon, "root")
        time.sleep(1.2)
        if "Password:" in read_log():
            type_line(mon, "admin")
            time.sleep(1.5)
            if "Welcome" in read_log():
                print("  [ok] logged in as root (attempt %d)" % (attempt + 1))
                return True
        # if we got here, either username or password failed -> loop retries
        print("  [retry] login attempt %d not confirmed, retrying" % (attempt + 1))
        time.sleep(0.8)
    return False


def type_gui(mon, s, delay=0.22):
    for ch in s:
        key = ("shift-%s" % ch.lower()) if 'A' <= ch <= 'Z' else ch
        send_key(mon, key, delay)


def screendump(mon, path_ppm):
    """Capture the framebuffer to a PPM file via the HMP `screendump` cmd."""
    mon.sendall(("screendump %s\n" % path_ppm).encode())
    # give QEMU a moment to flush the file
    time.sleep(0.35)
    # wait for the file to actually exist + have size
    for _ in range(20):
        if os.path.exists(path_ppm) and os.path.getsize(path_ppm) > 64:
            return True
        time.sleep(0.05)
    return False


def ppm_to_png(ppm, png):
    try:
        from PIL import Image
        Image.open(ppm).save(png)
        return png
    except Exception as e:  # noqa
        sys.stderr.write("ppm->png failed: %s\n" % e)
        return ppm


def main():
    if not os.path.exists(IMG):
        print("missing %s - run `make textboot` first" % IMG)
        sys.exit(2)

    os.makedirs(SHOT_DIR, exist_ok=True)
    for f in (LOG, ERR):
        open(f, "w").close()

    shots = []  # (label, png_path)

    errf = open(ERR, "wb")
    q = subprocess.Popen([
        "qemu-system-x86_64", "-m", "4096", "-vga", "std", "-display", "none",
        "-no-reboot",
        "-monitor", "tcp:127.0.0.1:%d,server,nowait" % PORT,
        "-chardev", "file,id=ser,path=%s" % LOG,
        "-serial", "chardev:ser",
        "-drive", "format=raw,file=%s" % IMG,
    ], stdout=errf, stderr=errf)

    def shot(name):
        ppm = os.path.join(SHOT_DIR, name + ".ppm")
        png = os.path.join(SHOT_DIR, name + ".png")
        if screendump(mon, ppm):
            p = ppm_to_png(ppm, png)
            shots.append((name, p))
            print("  [shot] %s -> %s" % (name, p))
        else:
            print("  [shot] %s FAILED (no framebuffer captured)" % name)

    try:
        mon = wait_sock(PORT)
        mon.settimeout(3.0)
        try:
            mon.recv(65536)
        except (TimeoutError, socket.timeout):
            pass

        print("[BOOT] waiting for text shell...")
        if not wait_for(BOOT_MARKERS, 60.0, "shell"):
            print("  [warn] boot marker not seen; continuing anyway")
        time.sleep(4.0)  # let the PS/2 keyboard controller settle

        print("[SHELL] login root / admin")
        if not login_root(mon):
            print("  [FATAL] could not log in; aborting test")
            mon.sendall(b"quit\n")
            time.sleep(1.0)
            q.terminate()
            sys.exit(3)
        time.sleep(1.0)

        print("[SHELL] gui  -> Win11 desktop 0")
        type_line(mon, "gui")
        time.sleep(10.0)
        shot("01_desktop0")

        print("[GUI] Ctrl+Right -> AI virtual desktop")
        send_key(mon, "ctrl-right", 2.0)
        shot("02_ai_desktop_idle")

        print("[AI DESK] type goal '%s' into chat box" % GOAL)
        type_word(mon, GOAL)
        time.sleep(1.8)
        shot("03_goal_typed")

        print("[AI DESK] Enter -> AiSend() + deferred agent run")
        send_key(mon, "ret", 0.10)
        # The agent run returns quickly in the VM; the typewriter then reveals
        # the reply at ~2 codepoints every 2 frames.  Capture a progression so
        # we can prove it is actually advancing rather than freezing.
        for dt, name in [(0.25, "04_thinking_a"),
                         (0.60, "05_thinking_b"),
                         (1.00, "06_typewriter_1"),
                         (3.00, "07_typewriter_2"),
                         (5.00, "08_typewriter_3"),
                         (8.00, "09_typewriter_done")]:
            time.sleep(dt)
            shot(name)

        # Let the typewriter fully finish and grab a clean full-reply frame.
        time.sleep(8.0)
        shot("12_full_reply")

        mon.sendall(b"quit\n")
        time.sleep(1.0)
    finally:
        try:
            q.wait(timeout=6.0)
        except subprocess.TimeoutExpired:
            q.terminate()
            try:
                q.wait(timeout=3.0)
            except subprocess.TimeoutExpired:
                q.kill()
    errf.close()

    txt = read_log()

    print("\n" + "=" * 70)
    print("AI virtual desktop LIVE test (headless QEMU + real screenshots)")
    print("=" * 70)
    print("\nScreenshots:")
    for name, p in shots:
        print("  %-22s %s" % (name, p))

    # Did the per-frame CLR fault that blanked the screen reappear?
    clr_faults = txt.count("[CLR] fault")
    clr_0x12 = txt.count("unsupported IL opcode 0x12")
    aid_run = "[AIDESK] run:" in txt
    ai_init_lines = [l.strip() for l in txt.splitlines() if "[AI]" in l][:12]

    print("\nSerial console checks:")
    print("  [AIDESK] run marker present : %s" % ("YES" if aid_run else "NO"))
    print("  total [CLR] fault lines     : %d" % clr_faults)
    print("  'unsupported IL opcode 0x12': %d" % clr_0x12)
    print("  TRIPLE FAULT / PANIC        : %s" % (
        "YES (BAD)" if ("TRIPLE FAULT" in txt.upper() or "PANIC" in txt.upper())
        else "no"))
    if ai_init_lines:
        print("\n  [AI] engine log (first lines):")
        for l in ai_init_lines:
            print("    " + l)

    # Show the AI desktop send line + any reply echo hints.
    for l in txt.splitlines():
        if "[AIDESK]" in l:
            print("    " + l.strip())

    # Capture the raw reply if the host surfaced it.
    print("\nRESULT: clr_0x12=%d aid_run=%s shots=%d" % (
        clr_0x12, aid_run, len(shots)))
    print("Done.")


if __name__ == "__main__":
    main()
