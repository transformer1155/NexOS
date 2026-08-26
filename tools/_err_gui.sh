#!/bin/bash
export PATH=/mingw32/bin:/usr/bin:$PATH
cd /d/MyOS/bootloader
make build/gui.o 2>&1 | grep -iE 'error:|undefined|note:' | head -40
echo "---exit---"
