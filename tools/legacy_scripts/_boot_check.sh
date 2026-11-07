#!/bin/bash
cd /media/sf_NexOS/NexOS || exit 1
cp build/os_v2.img /tmp/ai_slot.img
rm -f /tmp/ai_slot.log
timeout 75 qemu-system-x86_64 -accel tcg -drive format=raw,file=/tmp/ai_slot.img,snapshot=on -m 512 -serial file:/tmp/ai_slot.log -display none -no-reboot >/dev/null 2>&1
echo "serial bytes: $(stat -c%s /tmp/ai_slot.log 2>/dev/null || echo 0)"
echo '--- key lines ---'
grep -a -m1 'kmain64 entered\|K64' /tmp/ai_slot.log 2>/dev/null | head -2
grep -a -m1 'Entered GUI mode' /tmp/ai_slot.log 2>/dev/null
grep -a -m1 'initialisers' /tmp/ai_slot.log 2>/dev/null
grep -a -m1 'Network controller' /tmp/ai_slot.log 2>/dev/null
echo '--- last 3 non-empty lines ---'
grep -a . /tmp/ai_slot.log 2>/dev/null | tail -3
rm -f /tmp/ai_slot.img /tmp/ai_slot.log
echo 'cleanup verified:'
ls /tmp/ai_slot.img /tmp/ai_slot.log 2>&1 | head -2
