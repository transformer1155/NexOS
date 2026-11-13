#!/bin/bash
pkill -9 -f qemu-system 2>/dev/null; sleep 1
cd /media/sf_NexOS/NexOS || exit 1
# Copy the 429 MB image to the guest's LOCAL disk: QEMU raw reads over the
# VirtualBox shared folder are ~100x slower (per-sector vboxsf round trips),
# which alone can eat several minutes of the 2 MiB kernel64 staging read.
LOCAL=~/os_v2_local.img
if [ build/os_v2.img -nt "$LOCAL" ]; then
  echo "copying image to local disk..."
  cp -f build/os_v2.img "$LOCAL"
  sync
fi
sed -i "s|file=%s,format=raw|file=$LOCAL,format=raw|" tools/verify/test_boot64.py 2>/dev/null
python3 - <<'EOF'
import io
p = 'tools/verify/test_boot64.py'
s = io.open(p, encoding='utf-8').read()
if 'os_v2_local.img' not in s:
    s = s.replace('IMG  = os.path.join(ROOT, "build", "os_v2.img")',
                  'IMG  = os.path.expanduser("~/os_v2_local.img")')
    io.open(p, 'w', encoding='utf-8').write(s)
    print('test_boot64.py -> local image')
EOF
exec python3 tools/verify/test_boot64.py
