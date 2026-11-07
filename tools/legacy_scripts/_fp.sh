#!/bin/bash
openssl x509 -in /tmp/c3.pem -outform DER 2>/dev/null | sha256sum
openssl x509 -in /tmp/c3.pem -noout -fingerprint -sha256 2>/dev/null
openssl x509 -in /tmp/c2.pem -outform DER 2>/dev/null | sha256sum
