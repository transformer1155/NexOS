#!/bin/bash
set -e
cd /media/sf_NexOS/NexOS
# force image rebuild so the model embed step actually runs
rm -f build/os_v2.img
make MODEL_GGUF=build/qwen2-0_5b-instruct-q4_k_m.gguf 2>&1 | tail -10
ls -la build/os_v2.img
echo "=== BUILD DONE ==="
