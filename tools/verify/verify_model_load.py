#!/usr/bin/env python3
"""64-bit kernel `model load` path verification -- auto block.

The kernel64 build contains a TEMP VERIFY block right after login that runs
model_try_load() (MINIMDL1 blob -> big_alloc 986 MB -> ATA read -> qwen_load)
and qwen_chat() automatically, streaming to serial.  This script only needs
to get the 64-bit kernel logged in.

Assertions (serial):
  1. [K64-LOGIN] OK user=root        -- 64-bit shell reached
  2. [MVERIFY] auto model load start
  3. [MVERIFY] model loaded; answer: -- qwen_load succeeded + real inference
  4. [MVERIFY] done                  -- generation completed

Usage:  python3 verify_model_load.py [path/to/os_textboot.img]
"""
import os
import sys
import socket
import time
import subprocess
import shutil

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)

QEMU = os.environ.get("QEMU_BIN") or shutil.which("qemu-system-x86_64")
if not QEMU:
    for cand in ("/d/qemu/qemu-system-x86_64.exe",
                 "/c/Program Files/qemu/qemu-system-x86_64.exe",
                 "D:/qemu/qemu-system-x86_64.exe"):
        if os.path.exists(cand):
            QEMU = cand
            break
if not QEMU:
    print("ERROR: qemu-system-x86_64 not found")
    sys.exit(2)

IMG = sys.argv[1] if len(sys.argv) > 1 else "build/os_textboot.img"
MON_PORT = 4560
SER_LOG = "build/serial_model_load.log"


def wait_mon(port, timeout=30.0):
    end = time.time() + timeout
    while time.time() < end:
        try:
            return socket.create_connection(("127.0.0.1", port), timeout=0.5)
        except OSError:
            time.sleep(0.2)
    raise RuntimeError("monitor not ready: %d" % port)


def type_line(mon, s):
    keymap = {' ': 'spc', '.': 'dot', '/': 'slash', '\\': 'backslash',
              '-': 'minus', '_': 'shift-minus', '?': 'shift-slash'}
    for ch in s:
        key = "shift-%s" % ch.lower() if 'A' <= ch <= 'Z' else keymap.get(ch, ch)
        mon.sendall(("sendkey %s\n" % key).encode())
        time.sleep(0.05)
    mon.sendall(b"sendkey ret\n")
    time.sleep(0.3)


def read_log():
    if not os.path.exists(SER_LOG):
        return ""
    return open(SER_LOG, "rb").read().decode("latin-1", "ignore")


def wait_for(needle, timeout, desc):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if needle in read_log():
            print("  [ok] %s" % desc)
            return True
        time.sleep(1.0)
    print("  [XX] %s  (missing '%s')" % (desc, needle))
    return False


def main():
    if not os.path.exists(IMG):
        print("ERROR: image not found: %s" % IMG)
        return 1
    if os.path.exists(SER_LOG):
        open(SER_LOG, "w").close()

    print("=" * 66)
    print(" 64-bit kernel model load 验证 (MINIMDL1 -> qwen_load)")
    print(" 镜像: %s" % IMG)
    print("=" * 66)

    errf = open("build/qemu_model_load.err", "wb")
    qemu = subprocess.Popen([
        QEMU,
        "-drive", "format=raw,file=%s" % IMG,
        "-m", "1536M",
        "-vga", "std",
        "-display", "none",
        "-accel", "tcg,tb-size=8",
        "-monitor", "tcp:127.0.0.1:%d,server,nowait" % MON_PORT,
        "-chardev", "file,id=ser,path=%s" % SER_LOG,
        "-serial", "chardev:ser",
    ], stdout=errf, stderr=errf)

    ok1 = ok2 = ok3 = ok4 = False
    try:
        mon = wait_mon(MON_PORT)
        deadline = time.time() + 60.0
        while time.time() < deadline:
            if "login:" in read_log():
                break
            time.sleep(0.5)
        type_line(mon, "root")
        type_line(mon, "admin")
        time.sleep(1.0)

        print("[test] ask64 probe (switch to 64-bit)")
        type_line(mon, "ask64 probe")

        # kernel64 boots; wait for its login stage, retry credentials.
        deadline = time.time() + 180.0
        while time.time() < deadline:
            if read_log().count("[PERM] Y/N prompt engine armed") >= 2:
                break
            time.sleep(1.0)
        print("[test] kernel64 login stage; retrying credentials")
        deadline = time.time() + 150.0
        while time.time() < deadline:
            if "[K64-LOGIN]" in read_log():
                break
            type_line(mon, "root")
            type_line(mon, "admin")
            type_line(mon, "")
            time.sleep(4.0)
        ok1 = "[K64-LOGIN]" in read_log()
        print("  [%s] kernel64 登录" % ("ok" if ok1 else "XX"))

        # The TEMP VERIFY block runs model_try_load + qwen_chat automatically.
        ok2 = wait_for("[MVERIFY] auto model load start", 60, "验证块启动")
        ok3 = wait_for("[MVERIFY] model loaded; answer:", 600, "model load + 真实推理")
        ok4 = wait_for("[MVERIFY] done", 900, "推理完成")

        print("\n--- serial tail ---")
        lines = read_log().splitlines()
        print("\n".join(lines[-35:]))
    finally:
        try:
            qemu.terminate()
            qemu.wait(timeout=3.0)
        except Exception:
            try:
                qemu.kill()
            except Exception:
                pass
        errf.close()

    checks = [
        ("kernel64 登录", ok1),
        ("验证块启动", ok2),
        ("model load + 推理", ok3),
        ("推理完成", ok4),
    ]
    print("\n" + "=" * 66)
    for name, v in checks:
        print("  [%s] %s" % ("PASS" if v else "FAIL", name))
    print("=" * 66)
    all_ok = all(v for _, v in checks)
    print("RESULT:", "PASS" if all_ok else "FAIL",
          "(%d/4)" % sum(1 for _, v in checks if v))
    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(main())
