#!/bin/bash
cd /media/sf_NexOS/NexOS || exit 1
echo '=== .bss per object (mine vs market) ==='
for o in ai_config ai_tooluse ai_context ai_usage ai_kb ai_redact ai_coop ai_market; do
  f=build/${o}64.o
  if [ -f "$f" ]; then
    b=$(size -A "$f" 2>/dev/null | awk '/\.bss/{print $2}')
    t=$(size -A "$f" 2>/dev/null | awk '/\.text/{print $2}')
    printf '  %-12s .bss=%-8s .text=%-8s\n' "$o" "${b:-0}" "${t:-0}"
  fi
done
echo '=== top .bss objects in the 64-bit kernel ==='
for o in build/*64.o; do size -A "$o" 2>/dev/null | awk -v n="$o" '/\.bss/{if ($2>8192) print $2, n}'; done | sort -rn | head -12
echo '=== current .bss end from linker map (if any) ==='
grep -a 'HEAP_START\|PMM\] Initialised' /dev/null 2>/dev/null
echo '(need a boot log for that; skipped)'
