#!/usr/bin/env python3
"""Single-VM smoke test for 64-bit kernel real GGUF inference.
Uses QEMU loader device (0x501E = boot_no_gui) so the 32-bit kernel
auto-runs cmd_switch64() into the 64-bit server-mode kernel, which
runs AUTOTEST (load embedded GGUF + real qwen inference) at boot.
"""
import socket, time, subprocess, sys, os

QEMU = r"D:\qemu\qemu-system-x86_64.exe"
IMG  = r"d:\MyOS\bootloader\build\os.img"
SER  = r"d:\MyOS\bootloader\build\smoke_serial.log"
MON  = 4441

def main():
    if os.path.exists(SER):
        os.remove(SER)
    cmd = [
        QEMU, "-m", "2048", "-accel", "tcg",
        "-drive", "file=%s,format=raw,if=ide,index=0,media=disk" % IMG,
        "-net", "nic,model=ne2k_isa", "-net", "user",
        "-chardev", "file,id=ser,path=%s" % SER,
        "-serial", "chardev:ser",
        "-monitor", "tcp:127.0.0.1:%d,server,nowait" % MON,
        "-display", "none", "-no-reboot",
        "-device", "loader,addr=0x501E,data=1,data-len=1",  # boot_no_gui -> auto switch64
    ]
    print("launch:", " ".join(cmd))
    p = subprocess.Popen(cmd)
    try:
        print("loader 0x501E set; waiting for 64-bit AUTO switch64 + AUTOTEST...")
        for i in range(48):
            time.sleep(5)
            if os.path.exists(SER):
                t = open(SER, encoding="latin-1", errors="replace").read()
                if "AUTOTEST" in t and ("inference done" in t or "FAILED" in t):
                    print("AUTOTEST complete at ~%ds" % ((i+1)*5)); break
        time.sleep(2)
    finally:
        p.terminate()
        try: p.wait(timeout=5)
        except Exception: p.kill()
    print("serial ->", SER)

if __name__ == "__main__":
    main()
