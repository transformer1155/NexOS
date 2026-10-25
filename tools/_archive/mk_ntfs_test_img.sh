#!/usr/bin/env bash
# mk_ntfs_test_img.sh - build a real NTFS image for testing the NexOS NTFS driver.
# Requires: qemu-img, ntfs-3g (mkfs.ntfs / ntfscp / ntfsls). Runs under WSL.
set -e
IMG="${1:-/tmp/ntfs_test.img}"
SRC="$(mktemp -d)"
SIZE="${2:-64}"   # MB

mkdir -p "$SRC"
printf 'Hello from NexOS NTFS test file.\r\nSecond line of content.\r\n' > "$SRC/hello.txt"
printf '# README\r\nThis volume is used to validate the NexOS NTFS reader.\r\n' > "$SRC/readme.md"
mkdir -p "$SRC/sub"
printf 'nested file inside a subdirectory\r\n' > "$SRC/sub/nested.txt"
printf 'another root file to exercise listing\r\n' > "$SRC/notes.txt"

qemu-img create -f raw "$IMG" "${SIZE}M" >/dev/null
mkfs.ntfs -f -F "$IMG" >/dev/null 2>&1

MNT="$(mktemp -d)"
if mount -t ntfs-3g -o loop "$IMG" "$MNT" 2>/dev/null; then
    cp -r "$SRC/." "$MNT"/ 2>/dev/null
    umount "$MNT" 2>/dev/null || true
    echo "populated via FUSE loop mount: $IMG"
else
    echo "FUSE mount unavailable; using ntfscp (flat files only)"
    ntfscp "$IMG" "$SRC/hello.txt"  /hello.txt  2>/dev/null || true
    ntfscp "$IMG" "$SRC/readme.md"  /readme.md  2>/dev/null || true
    ntfscp "$IMG" "$SRC/notes.txt"  /notes.txt  2>/dev/null || true
    ntfscp "$IMG" "$SRC/sub/nested.txt" /nested.txt 2>/dev/null || true
fi
rmdir "$MNT" 2>/dev/null || true

# sanity: list with ntfsls
echo "--- ntfsls (reference) ---"
ntfsls "$IMG" 2>/dev/null || true
echo "IMAGE=$IMG"
