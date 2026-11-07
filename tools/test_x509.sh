#!/bin/bash
set -e
cd /media/sf_NexOS/NexOS
D=/tmp/x509t; rm -rf $D; mkdir -p $D; cd $D

openssl req -x509 -newkey rsa:2048 -keyout root.key -out root.pem -days 1 -nodes \
  -subj "/CN=Test Root" -addext "basicConstraints=critical,CA:TRUE" >/dev/null 2>&1
openssl req -newkey rsa:2048 -keyout leaf.key -out leaf.csr -nodes \
  -subj "/CN=localhost" >/dev/null 2>&1
openssl x509 -req -in leaf.csr -CA root.pem -CAkey root.key -CAcreateserial \
  -days 1 -out leaf.pem -extfile <(printf "subjectAltName=DNS:localhost") >/dev/null 2>&1

openssl x509 -in leaf.pem -outform DER -out leaf.der
openssl x509 -in root.pem -outform DER -out root.der
FP=$(openssl x509 -in root.pem -outform DER | sha256sum | cut -d' ' -f1)

python3 - "$FP" <<'EOF'
import sys
leaf=open('/tmp/x509t/leaf.der','rb').read()
root=open('/tmp/x509t/root.der','rb').read()
out=b''
for d in (leaf,root):
    out += len(d).to_bytes(3,'big') + d
open('/tmp/x509t/chain.bin','wb').write(out)
EOF

cd /media/sf_NexOS/NexOS
g++ -std=gnu++17 -O2 -I. -o $D/test_x509 tools/test_x509.cpp tls_x509.cpp tls_crypto.cpp 2>&1 | head -25
$D/test_x509 $D/chain.bin "$FP"
