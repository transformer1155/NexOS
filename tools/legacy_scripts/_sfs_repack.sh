#!/bin/bash
cd /media/sf_NexOS/NexOS || exit 1
echo '=== force repack SFS ==='
make build/sfs.img 2>&1 | tail -4
echo "sfs rc=${PIPESTATUS[0]}"
ls -l build/sfs.img | awk '{print "  sfs.img "$5" "$6" "$7" "$8}'
echo
echo '=== HARD GATE: templates must be inside the image ==='
a=$(grep -c -a 'bugfix-end-to-end' build/sfs.img)
b=$(grep -c -a '_invalid-probe' build/sfs.img)
echo "  bugfix-end-to-end : $a"
echo "  _invalid-probe    : $b"
if [ "$a" -ge 1 ] && [ "$b" -ge 1 ]; then echo 'GATE: PASS'; else echo 'GATE: FAIL -- do NOT boot'; fi
