#!/bin/bash
cd /media/sf_NexOS/NexOS 2>/dev/null || cd /media/sf_NexOS
echo '--- preflight ---'
bash /media/sf_NexOS/NexOS/tools/verify/preflight_idle.sh 2>/dev/null | tail -4
PF=$?
echo "preflight rc=$PF"
if [ "$PF" != "0" ]; then echo 'GATE BUSY -- not running verify_all'; exit 0; fi
echo '--- run verify_all (PATH for clang 21) ---'
export PATH=/usr/lib/llvm-21/bin:$PATH
cd /media/sf_NexOS/webemu || exit 1
timeout 900 bash tools/verify_all.sh 2>&1 | tail -40
echo "verify_all rc=${PIPESTATUS[0]}"
