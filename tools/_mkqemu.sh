#!/bin/bash
set -e
cd /mnt/d/MyOS/bootloader
TS=$(date +%s%N)
DST="build/os_textboot_qemu_${TS}.img"
cp build/os_textboot.img "$DST"
echo "BUILT_IMG=$DST"
ls -1 build/os_textboot_qemu_*.img | tail -1
