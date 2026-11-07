#!/bin/bash
cd /media/sf_NexOS/NexOS || exit 1
echo '--- [2] symbols in kernel64.elf ---'
for s in ai_config_parse ai_tooluse_run ai_context_reset ai_usage_quota_check ai_kb_add ai_redact_check; do
  n=$(nm build/kernel64.elf 2>/dev/null | grep -c " T $s")
  echo "  $s: $n"
done
echo '--- [3] must be 0 in 32-bit kernel.elf ---'
c=0
for s in ai_config_parse ai_tooluse_run ai_context_reset ai_usage_quota_check ai_kb_add ai_redact_check; do
  n=$(nm build/kernel.elf 2>/dev/null | grep -c " T $s")
  c=$((c+n))
done
echo "  32-bit total hits: $c"
echo '--- [4] embedded kernel64 byte-for-byte ---'
SZ=$(stat -c%s build/kernel64.bin)
echo "  kernel64.bin size: $SZ"
dd if=build/os_v2.img bs=1 skip=1048576 count=$SZ 2>/dev/null | cmp - build/kernel64.bin && echo '  KERNEL64: MATCH' || echo '  KERNEL64: DIFFERS'
echo '--- sizes ---'
ls -l build/kernel.bin build/kernel64.bin build/os_v2.img | awk '{print "  "$5" "$9}'
