#!/bin/bash
export PATH=/usr/lib/llvm-21/bin:$PATH
cd /media/sf_NexOS/webemu || exit 1
echo '--- image sync precheck (lead rule) ---'
bash /media/sf_NexOS/NexOS/tools/verify/preflight_image_sync.sh 2>&1 | tail -8
echo "preflight rc=$?"
echo
timeout 900 bash tools/verify_all.sh 2>&1
echo "verify_all rc=${PIPESTATUS[0]}"
