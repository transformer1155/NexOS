import os, subprocess, time, socket
ROOT = r"D:\MyOS\bootloader"
QEMU = r"D:\qemu\qemu-system-x86_64.exe"
CODE = r"D:\qemu\share\edk2-x86_64-code.fd"
VARS = os.path.join(ROOT, "build", "ovmf_vars_test.fd")
DISK = os.path.join(ROOT, "build", "os_uefi_voice.img")
SER = os.path.join(ROOT, "build", "serial_diag.txt")
open(SER, "w").close()
cmd = [QEMU, "-machine", "q35", "-m", "2048", "-accel", "tcg", "-vga", "none",
       "-device", "ramfb,id=rfb", "-display", "none",
       "-drive", f"if=pflash,format=raw,readonly=on,file={CODE}",
       "-drive", f"if=pflash,format=raw,file={VARS}",
       "-drive", f"file={DISK},format=raw,if=ide",
       "-serial", f"file:{SER}", "-monitor", f"tcp:127.0.0.1:4459,server,nowait"]
p = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
print("[diag] booting, capturing 40s serial...")
time.sleep(40)
try:
    s = socket.create_connection(("127.0.0.1", 4459), timeout=3)
    s.sendall(b"quit\n"); s.close()
except Exception as e:
    print("mon quit:", e)
try:
    p.wait(timeout=8)
except Exception:
    p.kill()
print("[diag] done; serial log size:", os.path.getsize(SER))
with open(SER, "r", errors="ignore") as f:
    txt = f.read()
print("=" * 60)
print(txt[-2500:])
