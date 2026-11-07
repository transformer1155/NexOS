#!/bin/bash
cd /media/sf_NexOS/NexOS || exit 1
echo '--- .o present? ---'
ls -l build/ai_coop64.o build/ai_market64.o 2>&1 | awk '{print "  "$5" "$9}'
echo '--- symbol in ai_coop64.o ---'
nm build/ai_coop64.o 2>/dev/null | grep -i 'ai_coop_install\|coop_run_local' | head -5
echo '--- compile ai_coop.cpp alone ---'
g++ -I. -m64 -mno-red-zone -mcmodel=kernel -ffreestanding -fno-exceptions -fno-rtti -fno-stack-protector -fno-pic -fno-pie -fcf-protection=none -fno-asynchronous-unwind-tables -nostdlib -O2 -fno-strict-aliasing -Wall -Wextra -fpermissive -DMINIOS_HAVE_MFORMS=1 -DNexOS_HAVE_MFORMS=1 -c ai_coop.cpp -o /tmp/x_coop.o 2>&1 | head -12
echo "compile rc=$?"
