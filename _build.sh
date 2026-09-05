#!/bin/bash
cd /mnt/d/MyOS/bootloader
make 2>&1 | grep -iE 'error|distnet|net\.cpp|undefined|agent|dn_agent' | head -60
echo "EXIT=${PIPESTATUS[0]}"
