#!/usr/bin/env bash
set -e
cd /mnt/e/NexOS/NexOS || exit 1
echo '=== 打包前 ==='
ls -l build/sfs.img
echo
echo '=== 运行 sfs_gen.py ==='
python3 tools/sfs_gen.py sfs_files build/sfs.img 2>&1 | tail -20
echo "rc=$?"
echo
echo '=== 打包后 ==='
ls -l build/sfs.img
stat -c 'size=%s mtime=%y' build/sfs.img
