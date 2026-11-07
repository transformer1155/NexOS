#!/bin/bash
cd /media/sf_NexOS/NexOS || exit 1
echo '--- make check-k64 ---'
make check-k64 2>&1 | tail -8
echo "check-k64 rc=$?"
echo '--- make -n adoption ---'
make -n build/kernel64.bin 2>/dev/null | grep -c 'ai_coop.cpp\|ai_market.cpp'
