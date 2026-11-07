#!/bin/bash
export PATH=/usr/lib/llvm-21/bin:$PATH
cd /media/sf_NexOS/webemu || exit 1
echo '--- first 16 bytes of shared-dir emu.wasm ---'
xxd -l 16 emu.wasm 2>/dev/null || od -An -tx1 -N16 emu.wasm
echo '--- is it all zeros? ---'
cmp -s emu.wasm <(head -c 54272 /dev/zero) && echo 'ALL ZEROS' || echo 'not all zeros'
echo '--- decisive: build into /tmp (non-shared) ---'
clang --target=wasm32 -nostdlib -O2 -Wl,--no-entry -Wl,--export-all -Wl,--export-memory -Wl,--allow-undefined -Wl,--initial-memory=100663296 -Wl,--max-memory=1073741824 src/cpu_min.c -o /tmp/emu_tmp.wasm 2>&1 | tail -5
echo "clang rc=${PIPESTATUS[0]}"
ls -l /tmp/emu_tmp.wasm 2>&1 | head -2
echo '--- /tmp result magic ---'
od -An -tx1 -N8 /tmp/emu_tmp.wasm 2>/dev/null
echo '--- shared dir result magic (again) ---'
od -An -tx1 -N8 emu.wasm 2>/dev/null
