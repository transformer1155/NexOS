#!/bin/bash
cd /media/sf_NexOS/NexOS || exit 1
echo '--- TLS symbols in kernel64.elf ---'
nm build/kernel64.elf 2>/dev/null | grep -E ' tls13_connect| x509_verify_chain| net_https_post| x25519| tls_crypto_seed' | head -10
echo '--- 32-bit kernel must NOT contain them ---'
nm build/kernel.elf 2>/dev/null | grep -cE ' tls13_connect| net_https_post' || true
echo '--- boot sanity (60s) ---'
cp build/os_v2.img /tmp/os_tls_test.img
A=tcg; C=''
if [ -c /dev/kvm ] && [ -r /dev/kvm ]; then A=kvm; C='-cpu host'; fi
rm -f /tmp/tls_boot.log
timeout 60 qemu-system-x86_64 -accel $A $C -drive format=raw,file=/tmp/os_tls_test.img -m 512 -serial file:/tmp/tls_boot.log -display none -no-reboot >/dev/null 2>&1
echo '--- serial tail ---'
tail -c 1200 /tmp/tls_boot.log
