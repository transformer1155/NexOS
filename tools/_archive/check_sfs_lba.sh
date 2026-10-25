#!/bin/bash
cd /mnt/e/NexOS/NexOS
echo "=== SFS magic 搜索 ==="
xxd build/os_v2.img | grep -c "5346 5300" 2>/dev/null
echo "---"
# 或者用 python 找
python3 -c "
with open('build/os_v2.img','rb') as f: d=f.read()
import re
offsets=[m.start() for m in re.finditer(b'SFS\x00', d)]
for o in offsets:
    lba=o//512
    print(f'SFS magic at byte {o} = LBA {lba} (offset from LBA start: {o%512})')
print(f'Total {len(offsets)} occurrences')
"
