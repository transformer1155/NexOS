#!/bin/bash
# Run the IME reproduction test inside the Linux build VM.
set -u
pkill -9 -f qemu-system 2>/dev/null || true
sleep 1
cd /media/sf_NexOS/NexOS || exit 1
exec python3 tools/verify/test_ime_v2.py "$@"
#!/bin/bash
# Run the IME reproduction test inside the Linux build VM.
set -u
pkill -9 -f qemu-system 2>/dev/null || true
sleep 1
cd /media/sf_NexOS/NexOS || exit 1
exec python3 tools/verify/test_ime_v2.py "$@"
#!/bin/bash
# Run the IME reproduction test inside the Linux build VM.
set -u
pkill -9 -f qemu-system 2>/dev/null || true
sleep 1
cd /media/sf_NexOS/NexOS || exit 1
exec python3 tools/verify/test_ime_v2.py "$@"
#!/bin/bash
# Run the IME reproduction test inside the Linux build VM.
set -u
pkill -9 -f qemu-system 2>/dev/null || true
sleep 1
cd /media/sf_NexOS/NexOS || exit 1
exec python3 tools/verify/test_ime_v2.py "$@"
