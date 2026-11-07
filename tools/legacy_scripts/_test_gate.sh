#!/bin/bash
cd /media/sf_NexOS/NexOS || exit 1
echo '=== TEST 1: normal path (expect rc=0, clean) ==='
make check-k64 2>&1 | tail -3
echo "  rc=${PIPESTATUS[0]}"
echo
echo '=== TEST 2: unwritable scratch -> expect GATE-IO-ERROR + rc=2 ==='
make check-k64 K64_STRICT_ELF=/tmp 2>&1 | tail -8
echo "  rc=${PIPESTATUS[0]}"
echo
echo '=== TEST 3: pre-existing non-writable scratch file ==='
touch /tmp/k64_sim_scratch.elf
chmod 444 /tmp/k64_sim_scratch.elf
make check-k64 K64_STRICT_ELF=/tmp/k64_sim_scratch.elf 2>&1 | tail -5
echo "  rc=${PIPESTATUS[0]}"
rm -f /tmp/k64_sim_scratch.elf
