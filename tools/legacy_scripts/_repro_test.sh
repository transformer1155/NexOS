#!/bin/bash
export PATH=/usr/lib/llvm-21/bin:$PATH
echo '=== syntax ==='
for f in /media/sf_NexOS/webemu/tools/build.sh /media/sf_NexOS/webemu/tools/verify_all.sh \
         /media/sf_NexOS/NexOS/tools/verify/assert_artifact_changed.sh \
         /media/sf_NexOS/NexOS/tools/verify/_selftest_artifact_changed.sh; do
  bash -n "$f" && echo "  OK $(basename $f)" || echo "  SYNTAX ERROR $(basename $f)"
done
echo
echo '=== REPRODUCIBILITY: build twice, compare sha ==='
cd /media/sf_NexOS/webemu || exit 1
bash tools/build.sh >/dev/null 2>&1; r1=$?
S1=$(sha256sum emu.wasm | cut -c1-32)
bash tools/build.sh >/dev/null 2>&1; r2=$?
S2=$(sha256sum emu.wasm | cut -c1-32)
echo "  run1 rc=$r1 sha=$S1"
echo "  run2 rc=$r2 sha=$S2"
if [ "$S1" = "$S2" ]; then echo '  REPRODUCIBLE: identical sha'; else echo '  NOT REPRODUCIBLE: sha differs'; fi
echo
echo '=== is a PID still embedded in the wasm? ==='
strings emu.wasm | grep -oE 'emu_build[^ ]*' | sort -u | head -3
echo "  (expect: emu_build.wasm, no digits)"
