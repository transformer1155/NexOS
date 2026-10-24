#!/bin/bash
cd /mnt/e/NexOS/NexOS
IMG="build/os_v2.img"
QEMU=/usr/bin/qemu-system-x86_64
rm -f _qemu_serial.log _qemu_monitor.log _qemu_frame.ppm

# 用 unix socket 作为 QMP monitor
$QEMU -M q35 -cpu qemu64 -m 2048   -drive file=$IMG,if=ide,index=0,media=disk   -vga std -net none -rtc base=localtime   -serial file:_qemu_serial.log   -monitor unix:/tmp/qemu_monitor.sock,server,nowait   -snapshot &
QPID=$!
echo "QEMU PID=$QPID"

# 等系统启动 (30s)
sleep 30

# 读 monitor socket 截帧
exec 3<>/tmp/qemu_monitor.sock
echo "screendump _qemu_frame.ppm" >&3
sleep 2
echo "quit" >&3
sleep 3
exec 3<&-
exec 3>&-

# kill 残留
kill $QPID 2>/dev/null
sleep 1

echo "=== Serial log tail ==="
tail -80 _qemu_serial.log 2>/dev/null
echo ""
echo "=== Frame size ==="
ls -la _qemu_frame.ppm 2>/dev/null
