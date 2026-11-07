#!/bin/bash
export PATH=/usr/lib/llvm-21/bin:$PATH
cd /media/sf_NexOS/webemu || exit 1
echo '--- rewritten _verify.sh head ---'
head -12 tools/_verify.sh
echo '--- emu.wasm present? ---'
ls -l emu.wasm 2>&1 | head -2
echo '--- clang version ---'
clang --version 2>&1 | head -2
echo '--- direct build (capture errors) ---'
bash tools/build.sh 2>&1 | tail -12
echo "build rc=${PIPESTATUS[0]}"
ls -l emu.wasm 2>&1 | head -2
