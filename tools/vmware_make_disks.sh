#!/bin/bash
# vmware_make_disks.sh -- build the VMware VMDKs from the current raw images.
#
# vmware-vdiskmanager -r refuses raw .img sources ("not a virtual disk"), so
# qemu-img is the converter of record here -- same tool tools/verify/to_vmware.sh
# already uses.  Run INSIDE the Linux build VM (shared folder mounted at
# /media/sf_NexOS/NexOS):
#     vm-run push tools/vmware_make_disks.sh /tmp/vmware_make_disks.sh
#     vm-run "bash /tmp/vmware_make_disks.sh"
set -e
R=/media/sf_NexOS/NexOS
OUT=$R/vmware/NexOS
mkdir -p "$OUT"

echo "[*] os_v2.img -> NexOS.vmdk (monolithicSparse, ide)"
qemu-img convert -f raw -O vmdk \
    -o subformat=monolithicSparse,adapter_type=ide \
    "$R/build/os_v2.img" "$OUT/NexOS.vmdk"

echo "[*] data_mkfs.raw -> NexOS-data.vmdk (monolithicSparse, ide)"
qemu-img convert -f raw -O vmdk \
    -o subformat=monolithicSparse,adapter_type=ide \
    "$R/build/data_mkfs.raw" "$OUT/NexOS-data.vmdk"

echo "[*] verify"
qemu-img info "$OUT/NexOS.vmdk"
qemu-img info "$OUT/NexOS-data.vmdk"

# sanity: the boot sector of the os disk must be a real MBR (0x55AA at 0x1FE)
echo "[*] mbr check:"
od -An -tx1 -j 510 -N 2 "$R/build/os_v2.img"
echo "[*] DONE"
