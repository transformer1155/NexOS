#!/bin/bash
cd /mnt/d/MyOS/Bootloader/tools
nohup python3 nexos_bridge.py > /tmp/bridge.log 2>&1 &
echo "BRIDGE_PID=$!"
sleep 2
echo "--- bridge log ---"
cat /tmp/bridge.log
echo "--- check 8765 ---"
python3 -c "import socket;s=socket.socket();s.settimeout(2);s.connect(('127.0.0.1',8765));print('WS_PORT 8765 OPEN');s.close()" 2>&1
