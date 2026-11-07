#!/bin/bash
cd /media/sf_NexOS/NexOS || exit 1
pkill -f 's_server -tls1_3' 2>/dev/null
sleep 0.3
D=/tmp/tls13t
mkdir -p $D
rm -f $D/cert.pem $D/key.pem
openssl req -x509 -newkey rsa:2048 -keyout $D/key.pem -out $D/cert.pem -days 1 -nodes -subj '/CN=localhost' -addext 'subjectAltName=DNS:localhost' >/dev/null 2>&1
PORT=4511
openssl s_server -tls1_3 -accept $PORT -cert $D/cert.pem -key $D/key.pem -www -quiet >$D/srv6.log 2>&1 &
SRV=$!
sleep 1.5
FP=$(openssl x509 -in $D/cert.pem -outform DER | sha256sum | cut -d' ' -f1)
g++ -std=gnu++17 -O2 -I. -DTLS13_HOST_TEST -o $D/client tools/test_tls13_host.cpp tls13.cpp tls_crypto.cpp tls_x509.cpp
if [ $? -ne 0 ]; then echo BUILD_FAILED; kill $SRV 2>/dev/null; exit 1; fi
$D/client $PORT 127.0.0.1 "$FP"
RC=$?
kill $SRV 2>/dev/null
echo "client rc=$RC"
exit $RC
