#!/bin/bash
export PATH=/usr/lib/llvm-21/bin:$PATH
cd /media/sf_NexOS/webemu || exit 1
echo '--- RESTORE emu.wasm (known-good tmp+cp pattern) ---'
clang --target=wasm32 -nostdlib -O2 -Wl,--no-entry -Wl,--export-all -Wl,--export-memory -Wl,--allow-undefined -Wl,--initial-memory=100663296 -Wl,--max-memory=1073741824 src/cpu_min.c -o /tmp/emu_restore.wasm 2>&1 | tail -3
cp /tmp/emu_restore.wasm emu.wasm
echo "  magic   : $(od -An -tx1 -N4 emu.wasm | tr -d ' \n')"
echo "  size    : $(stat -c%s emu.wasm)"
echo "  nonzero : $(tr -d '\000' < emu.wasm | wc -c)"
echo "  sha     : $(sha256sum emu.wasm | cut -c1-16)"
node -e "const fs=require('fs');const m=new WebAssembly.Module(fs.readFileSync('emu.wasm'));console.log('  imports='+WebAssembly.Module.imports(m).length+' exports='+WebAssembly.Module.exports(m).length);"
echo '--- build.sh line 25-33 (find the syntax error) ---'
sed -n '25,33p' tools/build.sh | cat -n
