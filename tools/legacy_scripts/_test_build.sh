#!/bin/bash
export PATH=/usr/lib/llvm-21/bin:$PATH
cd /media/sf_NexOS/webemu || exit 1
echo '--- record before ---'
sha256sum emu.wasm 2>/dev/null | cut -c1-16
stat -c '%s' emu.wasm 2>/dev/null
echo '--- run the FIXED build.sh ---'
bash tools/build.sh 2>&1 | tail -8
echo "build rc=${PIPESTATUS[0]}"
echo '--- verify the artifact after ---'
echo "  magic   : $(od -An -tx1 -N4 emu.wasm | tr -d ' \n')"
echo "  size    : $(stat -c%s emu.wasm)"
echo "  nonzero : $(tr -d '\000' < emu.wasm | wc -c)"
echo "  imports check via node:"
node -e "const fs=require('fs');const m=new WebAssembly.Module(fs.readFileSync('emu.wasm'));console.log('    imports='+WebAssembly.Module.imports(m).length+' exports='+WebAssembly.Module.exports(m).length);"
