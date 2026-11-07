#!/bin/bash
cd /media/sf_NexOS/NexOS || exit 1
echo '=== rebuild ai_coop64.o ==='
make build/ai_coop64.o 2>&1 | tail -3
echo "rc=${PIPESTATUS[0]}"
echo '=== new .bss for my modules ==='
tot=0
for o in ai_config ai_tooluse ai_context ai_usage ai_kb ai_redact ai_coop; do
  f=build/${o}64.o
  if [ -f "$f" ]; then
    b=$(size -A "$f" 2>/dev/null | awk '/\.bss/{print $2}'); b=${b:-0}
    tot=$((tot+b))
    printf '  %-11s .bss=%s\n' "$o" "$b"
  fi
done
echo "  --> mine total: $tot"
echo '=== ai_market for reference ==='
size -A build/ai_market64.o 2>/dev/null | awk '/\.bss/{print "  ai_market  .bss="$2}'
echo '=== gate ==='
make check-k64 2>&1 | tail -2
