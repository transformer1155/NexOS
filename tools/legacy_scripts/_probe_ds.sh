#!/bin/bash
echo | openssl s_client -connect api.deepseek.com:443 -servername api.deepseek.com 2>/dev/null > /tmp/ds.pem
openssl x509 -in /tmp/ds.pem -noout -subject -issuer 2>/dev/null
openssl x509 -in /tmp/ds.pem -noout -text 2>/dev/null | grep -E 'Signature Algorithm|Public Key Algorithm|DNS:' | head -8
echo '--- chain ---'
grep -c 'BEGIN CERTIFICATE' /tmp/ds.pem
