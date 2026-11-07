#!/bin/bash
cd /media/sf_NexOS/webemu || exit 1
echo "=== positive: real _dma.js (expect rc=0) ==="
node tools/_dma.js > /tmp/p_dma.out 2>&1; echo "  rc=$?"
grep -m1 'PASS' /tmp/p_dma.out | head -1
echo
echo '=== NEGATIVE: inject a fault (expect 0x11 -> 0x99) in a TEMP COPY ==='
cp tools/_dma.js /tmp/neg_dma.js
sed -i 's/got\[0\]===0x11/got[0]===0x99/' /tmp/neg_dma.js
grep -c 'got\[0\]===0x99' /tmp/neg_dma.js
node /tmp/neg_dma.js > /tmp/n_dma.out 2>&1; echo "  rc=$?"
grep -m1 'FAIL' /tmp/n_dma.out | head -1
echo
echo '=== NEGATIVE 2: same for _rx2.js (expect bytes[0]===1 -> 9) ==='
cp tools/_rx2.js /tmp/neg_rx2.js
sed -i 's/bytes\[0\]===1/bytes[0]===9/' /tmp/neg_rx2.js
node /tmp/neg_rx2.js > /tmp/n_rx.out 2>&1; echo "  rc=$?"
grep -m1 'FAIL' /tmp/n_rx.out | head -1
echo
echo '=== positives for the other two (expect rc=0) ==='
for s in _nettest.js _tx.js _rx2.js; do node tools/$s > /tmp/p_$s.out 2>&1; echo "  $s rc=$?"; done
rm -f /tmp/neg_dma.js /tmp/neg_rx2.js /tmp/p_*.out /tmp/n_*.out
