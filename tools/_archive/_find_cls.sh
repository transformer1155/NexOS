#!/bin/bash
cd /d/MyOS/bootloader
echo "=== closing braces between 1048 and 1535 ==="
awk 'NR>=1048 && NR<=1535 && /^[[:space:]]*\};/ {print NR": ["$0"]"}' gui.cpp
echo "=== lines 1525-1532 ==="
sed -n '1525,1532p' gui.cpp
