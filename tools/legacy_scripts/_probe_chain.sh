#!/bin/bash
echo | openssl s_client -connect api.deepseek.com:443 -servername api.deepseek.com -showcerts 2>/dev/null > /tmp/ds2.pem
echo -n 'certs in chain: '; grep -c 'BEGIN CERTIFICATE' /tmp/ds2.pem
echo '--- each cert ---'
awk '/BEGIN CERT/{n++} {if(n>0) print > ("/tmp/c" n ".pem")}' /tmp/ds2.pem
for f in /tmp/c1.pem /tmp/c2.pem /tmp/c3.pem; do
  [ -s $f ] || continue
  openssl x509 -in $f -noout -subject -issuer 2>/dev/null
  echo
done
