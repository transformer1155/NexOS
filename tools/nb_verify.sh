#!/bin/bash
cd /media/sf_NexOS/NexOS || exit 1
echo "--- fb_fill_rect present in kernel64.bin? ---"
grep -c "fb_fill_rect" build/kernel64.bin || echo "0 (inlined, expected)"
echo "--- kernel64.bin size ---"
ls -l build/kernel64.bin
echo "--- stage2 pixel_format store (c6 06 0f 50) ---"
python3 - <<'PY'
d=open('build/os_v2.img','rb').read()
print("stage2 0x500F store occurrences:", d.count(bytes([0xc6,0x06,0x0f,0x50])))
PY
echo "--- vbox_check ---"
python3 tools/vbox_check.py
