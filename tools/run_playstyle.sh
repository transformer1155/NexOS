#!/bin/bash
cd /mnt/e/NexOS/NexOS
rm -f _qemu_serial.log /tmp/mon.sock

# 完全按 Makefile play 的参数格式:
# $(QEMU) -drive format=raw,file=$(IMG) -m 4096 -vga std -display $(DISPLAY_BACKEND) -serial mon:stdio -no-reboot
# 我换成 -serial file:... 保存日志 + -monitor unix for screendump
/usr/bin/qemu-system-x86_64   -drive format=raw,file=build/os_v2.img   -m 4096   -vga std   -display none   -serial file:_qemu_serial.log   -monitor unix:/tmp/mon.sock,server,nowait   -no-reboot &
QPID=$!
echo "QPID=$QPID"

# 等 50s
sleep 50

# screendump
python3 -c "
import socket, time
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
for _ in range(10):
    try: s.connect('/tmp/mon.sock'); break
    except: time.sleep(0.5)
s.sendall(b'screendump _qemu_frame.ppm\n')
time.sleep(3)
s.sendall(b'quit\n')
s.close()
"

kill $QPID 2>/dev/null
sleep 2
echo ""
echo "=== Serial tail ==="
tail -60 _qemu_serial.log 2>/dev/null
