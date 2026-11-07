#!/bin/bash
export PATH=/usr/lib/llvm-21/bin:$PATH
cd /media/sf_NexOS/webemu || exit 1
echo '--- A. clang 直接写共享目录 ---'
clang --target=wasm32 -nostdlib -O2 -Wl,--no-entry -Wl,--export-all -Wl,--export-memory -Wl,--allow-undefined -Wl,--initial-memory=100663296 -Wl,--max-memory=1073741824 src/cpu_min.c -o /media/sf_NexOS/webemu/_a.wasm 2>&1 | tail -3
echo "  rc=${PIPESTATUS[0]}  size=$(stat -c%s _a.wasm 2>/dev/null)  magic=$(od -An -tx1 -N4 _a.wasm 2>/dev/null | tr -d ' ')"
echo '--- B. cp 到共享目录（同一文件系统写路径）---'
cp /tmp/emu_tmp.wasm /media/sf_NexOS/webemu/_b.wasm
echo "  size=$(stat -c%s _b.wasm)  magic=$(od -An -tx1 -N4 _b.wasm | tr -d ' ')"
echo '--- C. 写小文本文件到共享目录（对照）---'
echo hello > /media/sf_NexOS/webemu/_c.txt
echo "  content=$(cat /media/sf_NexOS/webemu/_c.txt)"
echo '--- D. 写 64KB 二进制到共享目录 ---'
head -c 65536 /tmp/emu_tmp.wasm > /media/sf_NexOS/webemu/_d.bin
echo "  size=$(stat -c%s _d.bin)  magic=$(od -An -tx1 -N4 _d.bin | tr -d ' ')"
echo '--- E. dd 54KB 到共享目录 ---'
dd if=/tmp/emu_tmp.wasm of=/media/sf_NexOS/webemu/_e.wasm bs=4096 2>/dev/null
echo "  size=$(stat -c%s _e.wasm)  magic=$(od -An -tx1 -N4 _e.wasm | tr -d ' ')"
rm -f /media/sf_NexOS/webemu/_a.wasm /media/sf_NexOS/webemu/_b.wasm /media/sf_NexOS/webemu/_c.txt /media/sf_NexOS/webemu/_d.bin /media/sf_NexOS/webemu/_e.wasm
