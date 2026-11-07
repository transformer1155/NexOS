#!/bin/bash
export PATH=/usr/lib/llvm-21/bin:$PATH
P="build/kernel.bin build/kernel64.bin build/sfs.img build/linux_sfs.img build/os_v2.img"
snap() { for f in $P; do printf '  %-22s %10s  %s  %s\n' "$(basename $f)" "$(stat -c%s $f)" "$(stat -c %y $f | cut -c1-19)" "$(sha256sum $f | cut -c1-16)"; done; }
cd /media/sf_NexOS/NexOS || exit 1
echo '=== NexOS products BEFORE ==='
snap
echo "  emu.wasm(webemu): $(stat -c%s /media/sf_NexOS/webemu/emu.wasm) $(sha256sum /media/sf_NexOS/webemu/emu.wasm | cut -c1-16)"
echo
cd /media/sf_NexOS/webemu || exit 1
timeout 900 bash tools/verify_all.sh 2>&1
echo "verify_all rc=${PIPESTATUS[0]}"
echo
cd /media/sf_NexOS/NexOS || exit 1
echo '=== NexOS products AFTER ==='
snap
echo "  emu.wasm(webemu): $(stat -c%s /media/sf_NexOS/webemu/emu.wasm) $(sha256sum /media/sf_NexOS/webemu/emu.wasm | cut -c1-16)"
