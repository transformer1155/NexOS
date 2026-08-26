#!/usr/bin/env bash
cd /d/MyOS/bootloader
make build/sfs.img > /tmp/sfs.log 2>&1
echo "SFS_EXIT=$?"
