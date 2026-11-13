#!/bin/bash
# Poll the detached build started by nb_build.sh.
LOG=${NB_LOG:-/tmp/mk.log}
if [ ! -f "$LOG" ]; then echo "no log yet ($LOG)"; exit 1; fi
if grep -q '^RC=' "$LOG" 2>/dev/null; then
    echo "=== DONE ==="
else
    echo "=== still running ==="
fi
grep -n '==> BIOS Image built\|^RC=\|make: \*\*\*\|Error 1\|Model:/|k64 undefined' "$LOG" 2>/dev/null | tail -12
echo "--- tail ---"
tail -3 "$LOG" 2>/dev/null
ls -l --time-style=full-iso /media/sf_NexOS/NexOS/build/os_v2.img 2>/dev/null
