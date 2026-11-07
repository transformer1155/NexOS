#!/bin/bash
cd /media/sf_NexOS/NexOS || exit 1
LD=.attic64/linker64.ld
cp "$LD" /tmp/lk64.bak

echo '########## POSITIVE: normal threshold 0x500000 ##########'
rm -f build/kernel64.elf
make build/kernel64.elf 2>&1 | tail -4
echo "POSITIVE rc=${PIPESTATUS[0]}"

echo '########## NEGATIVE: threshold forced to 0x400000 ##########'
sed -i 's/ASSERT(__bss_end <= 0x500000/ASSERT(__bss_end <= 0x400000/' "$LD"
grep -n 'ASSERT(__bss_end' "$LD"
rm -f build/kernel64.elf
make build/kernel64.elf 2>&1 | tail -6
echo "NEGATIVE rc=${PIPESTATUS[0]}"

echo '########## RESTORE ##########'
cp /tmp/lk64.bak "$LD"
grep -n 'ASSERT(__bss_end' "$LD"
rm -f build/kernel64.elf
make build/kernel64.elf 2>&1 | tail -3
echo "RESTORED rc=${PIPESTATUS[0]}"
rm -f /tmp/lk64.bak
