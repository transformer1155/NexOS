#!/bin/bash
cd /mnt/e/NexOS/NexOS
rm -f _qemu_serial.log _b.ppm _m.ppm /tmp/mon.sock

# 启动 QEMU (完全按 play target)
/usr/bin/qemu-system-x86_64   -drive format=raw,file=build/os_v2.img   -m 4096 -vga std -display none   -serial file:_qemu_serial.log   -monitor unix:/tmp/mon.sock,server,nowait   -no-reboot &
QPID=$!
echo "QPID=$QPID"

# 等 GUI (50s)
echo "Waiting 50s..."
sleep 50

# ==== 正确命令格式 ====
# QEMU monitor:
#   mouse_move X Y           (absolute position)
#   mouse_button 1           (left down)
#   mouse_button 2           (right down)  
#   mouse_button 0           (release all)
#   screendump FILENAME
#   quit

echo "=== Dismiss any menu ==="
echo "mouse_move 5 5"  | socat - UNIX-CONNECT:/tmp/mon.sock 2>/dev/null; sleep 0.3
echo "mouse_button 1"  | socat - UNIX-CONNECT:/tmp/mon.sock 2>/dev/null; sleep 0.2
echo "mouse_button 0"  | socat - UNIX-CONNECT:/tmp/mon.sock 2>/dev/null; sleep 0.5

echo "=== Capture BASE ==="
echo "screendump _b.ppm" | socat - UNIX-CONNECT:/tmp/mon.sock 2>/dev/null; sleep 2

echo "=== RIGHT-CLICK on wallpaper (640, 300) ==="
echo "mouse_move 640 300" | socat - UNIX-CONNECT:/tmp/mon.sock 2>/dev/null; sleep 0.3
echo "mouse_button 2"     | socat - UNIX-CONNECT:/tmp/mon.sock 2>/dev/null; sleep 0.3
echo "mouse_button 0"     | socat - UNIX-CONNECT:/tmp/mon.sock 2>/dev/null; sleep 4

echo "=== Capture MENU ==="
echo "screendump _m.ppm" | socat - UNIX-CONNECT:/tmp/mon.sock 2>/dev/null; sleep 2

echo "quit" | socat - UNIX-CONNECT:/tmp/mon.sock 2>/dev/null
sleep 2
kill $QPID 2>/dev/null
sleep 1

echo ""
echo "=== Serial markers ==="
grep -E "shell\.mex|MFORMS|GUI-STEP|menu|right" _qemu_serial.log 2>/dev/null | tail -15
echo ""
echo "=== Frame files ==="
ls -la _b.ppm _m.ppm 2>/dev/null || echo "FRAMES MISSING!"
