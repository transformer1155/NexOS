#!/bin/bash
cd /mnt/d/MyOS/Bootloader/tools
timeout 200 python3 test_e2e_stdlib.py 2>&1 | tail -40
