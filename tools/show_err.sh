#!/bin/sh
grep -B2 -A2 "CS0117" /tmp/build_out.txt | sort -u > /tmp/e.txt
cat /tmp/e.txt
echo ----all-unique-errors----
grep "error" /tmp/build_out.txt | sort -u | tail -6
