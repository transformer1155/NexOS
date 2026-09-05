#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Drive a NexOS VM in VirtualBox via keyboard scancode injection.

VBox has no HMP monitor like QEMU, so we inject PS/2 scancode sequences with
`VBoxManage controlvm NexOS keyboardputscancode`.  Flow:

    login root/admin -> switch (64-bit) -> login -> agent run <question>
    -> the built-in Qwen GGUF answers (hardware-accelerated in VBox).

Usage:  python tools/vbox_drive.py "what is 2+2"
"""
import subprocess
import sys
import time
import os

VBM = r"E:\Program Files\Oracle\VirtualBox\VBoxManage.exe"
VM = "NexOS"
LOG = r"D:\MyOS\bootloader\build\serial_vbox.log"

# Scan code set 1 (make codes); break = make | 0x80
KEYS = {
    'a':0x1E,'b':0x30,'c':0x2E,'d':0x20,'e':0x12,'f':0x21,'g':0x22,'h':0x23,
    'i':0x17,'j':0x24,'k':0x25,'l':0x26,'m':0x32,'n':0x31,'o':0x18,'p':0x19,
    'q':0x10,'r':0x13,'s':0x1F,'t':0x14,'u':0x16,'v':0x2F,'w':0x11,'x':0x2D,
    'y':0x15,'z':0x2C,
    '0':0x0B,'1':0x02,'2':0x03,'3':0x04,'4':0x05,'5':0x06,'6':0x07,'7':0x08,
    '8':0x09,'9':0x0A,
    ' ':0x39,'\n':0x1C,'.':0x34,'/':0x35,"'":0x28,';':0x27,'-':0x0C,'=':0x0D,
    ',':0x33,'+':0x0D,'?':0x35,'!':0x02,'(':0x0A,')':0x0B,'_':0x0C,':':0x27,
}
SHIFT_KEYS = set("?()_!:")


def kscan(ch):
    """Return list of scancode bytes for one character."""
    out = []
    if 'A' <= ch <= 'Z':
        out += [0x2A]                      # shift down
        out += [KEYS[ch.lower()], KEYS[ch.lower()] | 0x80]
        out += [0xAA]                      # shift up
    elif ch in SHIFT_KEYS:
        out += [0x2A]
        out += [KEYS[ch], KEYS[ch] | 0x80]
        out += [0xAA]
    else:
        c = KEYS.get(ch)
        if c is None:
            return []
        out += [c, c | 0x80]
    return out


def inject(text, pause=0.06):
    for ch in text:
        seq = kscan(ch)
        if not seq:
            continue
        args = " ".join("%02X" % b for b in seq)
        subprocess.run([VBM, "controlvm", VM, "keyboardputscancode"] + [x for x in args.split()],
                       capture_output=True)
        time.sleep(pause)
    # Enter
    subprocess.run([VBM, "controlvm", VM, "keyboardputscancode", "1C", "9C"],
                   capture_output=True)
    time.sleep(0.3)


def rd():
    try:
        with open(LOG, "rb") as f:
            return f.read().decode("latin-1", "ignore")
    except OSError:
        return ""


def wait_for(markers, timeout, step=0.5):
    end = time.time() + timeout
    while time.time() < end:
        t = rd()
        if any(m in t for m in markers):
            return True
        time.sleep(step)
    return False


def main():
    goal = " ".join(sys.argv[1:]) or "what is 2+2"
    print("[1] waiting for login...")
    if not wait_for(["login"], 60):
        print("  no login prompt; dump:"); print(rd()[-800:]); return 1
    time.sleep(2)
    inject("root")
    time.sleep(2)
    inject("admin")
    time.sleep(3)
    t = rd()
    print("  32-bit logged in:", "Welcome" in t)
    print("[2] switch -> 64-bit...")
    inject("switch")
    if not wait_for(["K64 image staged", "Loading kernel64"], 30):
        print("  switch not staged; tail:"); print(rd()[-500:]); return 1
    wait_for(["login"], 40)
    time.sleep(2)
    inject("root")
    time.sleep(2)
    inject("admin")
    time.sleep(3)
    print("  64-bit shell ready:", "Welcome" in rd())
    print("[3] agent run %r (model loads lazily, then answers)..." % goal)
    inject("agent run " + goal)
    ok = wait_for(["[qwen] ask:"], 120)
    print("  qwen ask started:", ok)
    done = wait_for(["[qwen] tokens="], 600)
    print("  qwen done:", done)
    t = rd()
    si = t.find("[qwen] out:")
    ei = t.find("[qwen] tokens=")
    if si >= 0 and ei > si:
        ans = t[si + 10:ei]
        print("\n=== ANSWER (VirtualBox, hardware-accelerated) ===")
        print(ans[:900])
        print("=" * 50)
        with open(r"D:\MyOS\bootloader\build\vbox_answer.txt", "w") as f:
            f.write(ans)
        print("saved to build/vbox_answer.txt")
    return 0


if __name__ == "__main__":
    sys.exit(main())
