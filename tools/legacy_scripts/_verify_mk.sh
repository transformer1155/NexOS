#!/bin/bash
cd /media/sf_NexOS/NexOS || exit 1
echo '--- [1] grep -n ai_coop64 / ai_market64 ---'
grep -n 'ai_coop64\|ai_market64' Makefile
echo '--- [2] make -n 采纳检查 ---'
make -n build/kernel64.bin 2>/dev/null | grep -c 'ai_coop.cpp\|ai_market.cpp'
echo '--- [3] sanity: 32-bit link must NOT reference them ---'
grep -c 'ai_coop64\|ai_market64' Makefile
echo '--- [4] files exist ---'
ls -l ai_coop.cpp ai_coop.h ai_market.cpp ai_market.h 2>&1 | awk '{print "  "$5" "$9}'
