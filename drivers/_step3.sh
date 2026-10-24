#!/usr/bin/env bash
set -e
cd /mnt/e/NexOS/NexOS || exit 1
B=build
IMG=$B/os_v2.img
echo '=== 3a. 组装 boot + stage2 + kernel32 ==='
cat $B/boot.bin $B/stage2.bin $B/kernel.bin > $IMG
truncate -s 4M $IMG
echo "after cat+truncate: $(stat -c%s $IMG) B"

echo '=== 3b. kernel64 @ LBA 2048 ==='
dd if=$B/kernel64.bin of=$IMG bs=512 seek=2048 conv=notrunc 2>/dev/null
echo "kernel64 $(stat -c%s $B/kernel64.bin) B -> LBA 2048"

echo '=== 3c. SFS @ LBA 6144 ==='
dd if=$B/sfs.img of=$IMG bs=512 seek=6144 conv=notrunc 2>/dev/null
echo "sfs $(stat -c%s $B/sfs.img) B -> LBA 6144"

echo '=== 3d. linux_sfs @ LBA 25600 ==='
dd if=$B/linux_sfs.img of=$IMG bs=512 seek=25600 conv=notrunc 2>/dev/null
echo "linux_sfs $(stat -c%s $B/linux_sfs.img) B -> LBA 25600"

echo '=== 最终镜像 ==='
ls -l $IMG
stat -c 'size=%s mtime=%y' $IMG
