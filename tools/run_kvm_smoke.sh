#!/bin/bash
# tools/run_kvm_smoke.sh - build and run the KVM guest smoke test.
#
# Validates the minimal guest program (plugins/guest_smoke.asm) on a real
# hypervisor (KVM), proving it runs, paints the shared framebuffer, emits
# "ready" on the virtual UART, and exits cleanly.
#
# /dev/kvm is usually root:kvm 0660.  This script tries, in order:
#   1. direct access (if the user is in the kvm group),
#   2. sudo -n (passwordless sudo),
#   3. sudo (interactive).
set -uo pipefail
cd "$(dirname "$0")/.." || exit 1

BLOB=plugins/guest_smoke.bin
HARNESS=/tmp/kvm_smoke

echo "=== building guest blob and harness ==="
nasm -f bin plugins/guest_smoke.asm -o "$BLOB" || { echo "nasm failed"; exit 1; }
echo "blob: $(wc -c < "$BLOB") bytes"

gcc -O2 -Wall -Wextra -o "$HARNESS" tools/kvm_smoke.c || { echo "gcc failed"; exit 1; }
echo "harness: $HARNESS"

# Can we open /dev/kvm directly?
if python3 -c "import os;os.close(os.open('/dev/kvm',os.O_RDWR))" 2>/dev/null; then
    echo "--- /dev/kvm directly accessible ---"
    exec "$HARNESS" "$BLOB"
fi

echo "--- /dev/kvm needs elevated access ---"
if sudo -n true 2>/dev/null; then
    exec sudo -n "$HARNESS" "$BLOB"
fi

echo "--- requesting sudo (may prompt for your password) ---"
exec sudo "$HARNESS" "$BLOB"
