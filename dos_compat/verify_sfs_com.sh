#!/usr/bin/env bash
# Verify hello.com is present inside the generated SFS volume.
set -uo pipefail
cd "$(dirname "$0")/.."

ls -la sfs_files/hello.com || exit 1
echo "--- scanning build/sfs.img ---"
python3 - <<'PY'
d = open('build/sfs.img', 'rb').read()
for probe in (b'hello.com', b'Hello DOS', b'\xcd\x21'):  # name, payload, INT 21h opcode
    i = d.find(probe)
    print(f"{probe!r:16} -> {'offset ' + str(i) if i >= 0 else 'NOT FOUND'}")
PY