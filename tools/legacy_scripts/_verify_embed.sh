#!/bin/bash
cd /media/sf_NexOS/NexOS || exit 1
SZ=$(stat -c%s build/kernel64.bin)
echo "kernel64.bin size: $SZ"
# Block reads + exact truncation.  bs=1 here meant ~1,035,000 single-byte read() calls;
# on vboxsf that never finishes (same bug that hung preflight_image_sync.sh).  The SFS
# line below already used bs=512 -- this line just did not follow it.
SZSEC=$(( (SZ + 511) / 512 ))
if dd if=build/os_v2.img bs=512 skip=2048 count=$SZSEC 2>/dev/null | head -c "$SZ" | cmp -s - build/kernel64.bin; then
  echo 'KERNEL64: MATCH'
else
  echo 'KERNEL64: DIFFERS'
fi
echo '--- SFS embed ---'
# 18590 was a hand-copied sector count: it silently goes short the moment sfs.img grows,
# which would read as DIFFERS.  Derive it instead.
SZ2=$(stat -c%s build/sfs.img); SZ2SEC=$(( (SZ2 + 511) / 512 ))
if dd if=build/os_v2.img bs=512 skip=6144 count=$SZ2SEC 2>/dev/null | head -c "$SZ2" | cmp -s - build/sfs.img; then echo 'SFS: MATCH'; else echo 'SFS: differs-or-size'; fi
echo '--- sizes ---'
stat -c '%s %n' build/kernel.bin build/kernel64.bin build/os_v2.img
