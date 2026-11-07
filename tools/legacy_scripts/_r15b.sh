#!/bin/bash
export PATH=/usr/lib/llvm-21/bin:$PATH
cd /media/sf_NexOS/webemu || exit 1
echo '--- artifact identity BEFORE run ---'
echo "  emu.wasm   size=$(stat -c%s emu.wasm) mtime=$(stat -c %y emu.wasm | cut -c1-19) sha16=$(sha256sum emu.wasm | cut -c1-16)"
echo "  cpu_min.c  size=$(stat -c%s src/cpu_min.c) mtime=$(stat -c %y src/cpu_min.c | cut -c1-19)"
echo "  magic=$(od -An -tx1 -N4 emu.wasm | tr -d ' \n')  nonzero=$(tr -d '\000' < emu.wasm | wc -c)"
echo
timeout 900 bash tools/verify_all.sh 2>&1
echo "rc=${PIPESTATUS[0]}"
echo '--- artifact identity AFTER run ---'
echo "  emu.wasm sha16=$(sha256sum emu.wasm | cut -c1-16)"
