#!/usr/bin/env bash
echo '--- multilib check ---'
echo 'int main(){return 0;}' > /tmp/t.c
gcc -m32 /tmp/t.c -o /tmp/t32 2>&1 | head -5 && echo M32_OK || echo M32_FAIL
ls -l /tmp/t32 2>/dev/null
