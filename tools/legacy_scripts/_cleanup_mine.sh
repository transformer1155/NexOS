#!/bin/bash
# Clean ONLY artifacts this agent created. Explicit names, no wildcards, then verify.
echo '--- before ---'
df -h /tmp | tail -1
ls -l /tmp/os_tls_test.img /tmp/tls_boot.log 2>/dev/null
du -sh /tmp/tls13t /tmp/x509t /tmp/pss 2>/dev/null

MINE="/tmp/os_tls_test.img /tmp/tls_boot.log /tmp/dbg_sig.bin /tmp/dbg_th.bin /tmp/dbg_signed.bin /tmp/dbg_pub.pem /tmp/mk.log /tmp/_t13.sh /tmp/_t13v2.sh /tmp/_tac.sh /tmp/_x509.sh /tmp/_pss.sh /tmp/_md.sh /tmp/_xd.sh /tmp/_vt.sh /tmp/_fp.sh /tmp/_pc.sh /tmp/_pds.sh /tmp/_rct.sh /tmp/_mk.sh /tmp/_d13.sh /tmp/_a13.sh"
for f in $MINE; do rm -f -- "$f"; done
rm -rf -- /tmp/tls13t /tmp/x509t /tmp/pss

echo '--- after (verify by listing, not by trusting rm) ---'
LEFT=0
for f in $MINE; do [ -e "$f" ] && { echo "STILL PRESENT: $f"; LEFT=$((LEFT+1)); }; done
for d in /tmp/tls13t /tmp/x509t /tmp/pss; do [ -e "$d" ] && { echo "STILL PRESENT: $d"; LEFT=$((LEFT+1)); }; done
echo "remaining_mine=$LEFT"
df -h /tmp | tail -1
echo '--- others copies left alone (do not touch) ---'
ls -l /tmp/svmprobe.img /tmp/os_k.img /tmp/os_plain.img /tmp/os_dos.img 2>/dev/null
