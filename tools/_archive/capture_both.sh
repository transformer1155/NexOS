#!/bin/bash
cd /mnt/e/NexOS/NexOS
rm -f _qemu_serial.log _qemu_base.ppm _qemu_menu.ppm /tmp/mon.sock

/usr/bin/qemu-system-x86_64   -drive format=raw,file=build/os_v2.img   -m 4096 -vga std -display none   -serial file:_qemu_serial.log   -monitor unix:/tmp/mon.sock,server,nowait   -no-reboot &
QPID=$!
echo "QPID=$QPID"

echo "Waiting 50s..."
sleep 50

# 1. 截 base frame
echo "=== Step 1: base frame ==="
echo "screendump _qemu_base.ppm" | socat - UNIX-CONNECT:/tmp/mon.sock 2>/dev/null
sleep 2

# 2. 移动鼠标到桌面中央，发右键
echo "=== Step 2: send right-click ==="
echo "mouse_move 640 300" | socat - UNIX-CONNECT:/tmp/mon.sock 2>/dev/null
sleep 0.3
echo "mouse button right" | socat - UNIX-CONNECT:/tmp/mon.sock 2>/dev/null
sleep 0.2
echo "mouse button right" | socat - UNIX-CONNECT:/tmp/mon.sock 2>/dev/null
sleep 3

# 3. 截 menu frame
echo "=== Step 3: menu frame ==="
echo "screendump _qemu_menu.ppm" | socat - UNIX-CONNECT:/tmp/mon.sock 2>/dev/null
sleep 2

# 4. kill
echo "quit" | socat - UNIX-CONNECT:/tmp/mon.sock 2>/dev/null
sleep 2
kill $QPID 2>/dev/null
sleep 1

echo ""
echo "=== Files ==="
ls -la _qemu_base.ppm _qemu_menu.ppm 2>/dev/null
