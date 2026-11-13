#!/bin/bash
set -e
qemu-img convert -f raw -O vmdk -o subformat=monolithicSparse /media/sf_NexOS/NexOS/build/os_v2.img /media/sf_NexOS/NexOS/build/os_v2_vmware.vmdk
qemu-img info /media/sf_NexOS/NexOS/build/os_v2_vmware.vmdk
echo "=== CONVERT DONE ==="
