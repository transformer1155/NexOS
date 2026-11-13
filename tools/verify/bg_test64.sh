#!/bin/bash
# Background runner: boot64 QEMU end-to-end test, logs into the shared folder.
pkill -9 -f qemu-system 2>/dev/null
sleep 1
cd /media/sf_NexOS/NexOS || exit 1
cp -f build/os_v2.img ~/os_v2_local.img
cp -f build/qwen2-0_5b-instruct-q4_k_m.gguf ~/qwen_model.raw
sync
python3 tools/verify/test_boot64.py > build/boot64_out.log 2>&1
echo EXITCODE=$? >> build/boot64_out.log
