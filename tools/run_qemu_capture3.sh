#!/bin/bash
cd /mnt/e/NexOS/NexOS
rm -f _qemu_serial.log _qemu_frame.ppm /tmp/qmp.sock /tmp/mon.log

/usr/bin/qemu-system-x86_64 -M q35 -cpu qemu64 -m 2048   -drive file=build/os_v2.img,if=ide,index=0,media=disk   -vga std -net none -rtc base=localtime   -serial file:_qemu_serial.log   -monitor unix:/tmp/qmp.sock,server,nowait   -display none -snapshot &
QPID=$!
echo "QPID=$QPID"

# 等 GUI 完全启动 (45s - shell.mex + SFS mount + GUI init)
echo "Waiting 45s for GUI..."
sleep 45

# 用 socat 或 python 发命令到 monitor
if command -v socat >/dev/null; then
  echo "screendump _qemu_frame.ppm" | socat - UNIX-CONNECT:/tmp/qmp.sock
  sleep 2
else
  exec 3<>/tmp/qmp.sock
  echo "screendump _qemu_frame.ppm" >&3
  sleep 2
  exec 3<&-
fi

kill $QPID 2>/dev/null
sleep 1

echo "=== Done ==="
ls -la _qemu_frame.ppm _qemu_serial.log 2>/dev/null
echo ""
echo "=== Serial tail ==="
tail -50 _qemu_serial.log 2>/dev/null
