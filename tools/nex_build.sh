#!/bin/bash
set -o pipefail
cd /mnt/e/NexOS/NexOS 2>/dev/null || cd "$1"
make build/os_v2.img MODEL_GGUF=build/qwen2-0_5b-instruct-q4_k_m.gguf > /tmp/nexbuild.log 2>&1
rc=$?
echo "MAKE_RC=$rc" >> /tmp/nexbuild.log
grep -nE "standalone app|Error\(s\)|error:|k64 undefined|Image built|Time Elapsed|make: \*\*\*" /tmp/nexbuild.log | tail -60
exit 0
