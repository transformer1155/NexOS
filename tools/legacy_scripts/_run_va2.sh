#!/bin/bash
export PATH=/usr/lib/llvm-21/bin:$PATH
cd /media/sf_NexOS/webemu || exit 1
timeout 900 bash tools/verify_all.sh 2>&1 | tail -30
echo "verify_all rc=${PIPESTATUS[0]}"
