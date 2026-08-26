#!/usr/bin/env python3
import sys
p = sys.argv[1] if len(sys.argv) > 1 else r"d:\MyOS\bootloader\build\smoke_serial.log"
with open(p, "r", encoding="latin-1", errors="replace") as f:
    txt = f.read()
outp = sys.argv[2] if len(sys.argv) > 2 else r"d:\MyOS\bootloader\build\smoke_dump.txt"
with open(outp, "w", encoding="utf-8", errors="replace") as o:
    o.write("LEN=%d\n" % len(txt))
    # Print whole log but mark interesting markers
    o.write(txt)
    o.write("\n=== MARKERS ===\n")
    for m in ["stage[0..3]", "staged", "S d M", "long mode", "6 a e", "kmain64", "K64-1", "AUTOTEST"]:
        o.write("%s : %s\n" % (m, m in txt))
print("written to", outp)
