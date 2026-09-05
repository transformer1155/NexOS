#!/bin/bash
cd /mnt/d/MyOS/Bootloader/tools
pkill -f nexos_bridge.py 2>/dev/null
sleep 1
timeout 200 python3 test_e2e_stdlib.py 2>&1 | tail -45
