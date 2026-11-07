#!/bin/bash
export PATH=/usr/lib/llvm-21/bin:$PATH
cd /media/sf_NexOS/webemu || exit 1
echo '=== run build.sh (symbol consistency assertion should catch main.js) ==='
bash tools/build.sh 2>&1 | tail -12
echo "build rc=${PIPESTATUS[0]}"
echo '--- what does main.js actually reference? ---'
grep -oE '\bex\.[A-Za-z_][A-Za-z0-9_]*\s*\(' js/main.js | sed 's/[[:space:]]*($//' | sort -u | head -20
echo '--- does wasm export mem_base? ---'
node -e "const fs=require('fs');const m=new WebAssembly.Module(fs.readFileSync('emu.wasm'));const e=WebAssembly.Module.exports(m).map(x=>x.name);console.log('  mem_base present:', e.includes('mem_base'), ' ram_base present:', e.includes('ram_base'));"
