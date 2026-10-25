#!/bin/bash
df -h /tmp | tail -1
echo "--- WSL swap ---"
ls -la /mnt/c/Users/trans/AppData/Local/temp/*/swap.vhdx 2>/dev/null || echo "no swap vhdx visible"
