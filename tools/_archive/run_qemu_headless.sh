#!/bin/bash
cd /mnt/e/NexOS/NexOS
IMG="build/os_v2.img"
QEMU=/usr/bin/qemu-system-x86_64
rm -f _qemu_serial.log qemu_monitor.txt
$QEMU -M q35 -cpu qemu64 -m 2048   -drive file=$IMG,if=ide,index=0,media=disk   -vga std -net none -rtc base=localtime   -serial file:_qemu_serial.log   -monitor file:qemu_monitor.txt   -display none   -snapshot &
echo "QEMU started PID=$!"
sleep 3
ps aux | grep qemu | grep -v grep | head -3
