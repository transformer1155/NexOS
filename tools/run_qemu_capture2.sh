#!/bin/bash
cd /mnt/e/NexOS/NexOS
rm -f _qemu_serial.log /tmp/qmp.sock

# 启动 QEMU, headless, VNC 在 5900（方便 savevm）
# 或者直接用 -monitor telnet 方式
/usr/bin/qemu-system-x86_64 -M q35 -cpu qemu64 -m 2048   -drive file=build/os_v2.img,if=ide,index=0,media=disk   -vga std -net none -rtc base=localtime   -serial file:_qemu_serial.log   -monitor telnet:127.0.0.1:4444,server,nowait   -display none   -snapshot &
QPID=$!
echo "QEMU PID=$QPID"

# 等 GUI 起来 (40s)
sleep 40

# 通过 telnet 发 savevm
(echo -e "screendump _qemu_frame.ppm
exit
" | nc -q 2 127.0.0.1 4444) 2>/dev/null ||   (echo -e "screendump _qemu_frame.ppm
exit
" | telnet 127.0.0.1 4444 2>/dev/null) ||   echo "screendump via nc failed"

sleep 3
kill $QPID 2>/dev/null
sleep 1
echo "done"
