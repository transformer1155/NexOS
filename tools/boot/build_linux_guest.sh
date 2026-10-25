#!/usr/bin/env bash
# =====================================================================
#  build_linux_guest.sh - build the Linux Guest root image for the
#  NexOS built-in VMM (bare-metal "Linux in a NexOS window").
# =====================================================================
#
#  WHAT THIS PRODUCES
#  ------------------
#  A raw disk image (default: build/linux_guest.img) that the NexOS VMM
#  attaches as the guest's vDisk.  Inside it lives a minimal, bootable
#  Linux with a framebuffer console, so the shared in-RAM framebuffer
#  (vmm.cpp: vmm_guest_fb) shows a real Linux desktop.
#
#  WHY A RAW IMAGE AND NOT AN ISO
#  ------------------------------
#  The VMM exposes a block device, not a CD-ROM: vDisk serves 512-byte
#  sectors from a flat file.  A raw ext2/4 image can be attached directly
#  and read with the same "seek to lba*512, read count*512" logic the
#  kernel already uses for its own disks.
#
#  WHAT IT NEEDS ON THE BUILD HOST
#  -------------------------------
#  Standard tools only:  curl/wget, mkfs.ext2, mcopy/mdir (mtools),
#  optionally qemu-nbd or a loop mount (needs root) for finer control.
#
#  The kernel comes from the host distro's /boot (or an explicit path).
#  The userspace is BusyBox static, which is enough for a serial/framebuffer
#  console and for running arbitrary Linux binaries later.
#
#  USAGE
#  -----
#    ./tools/boot/build_linux_guest.sh                       # auto-detect
#    ./tools/boot/build_linux_guest.sh -k /boot/vmlinuz -s 256   # 256 MiB image
#    NEXOS_LINUX_KERNEL=/path/vmlinuz ./tools/boot/build_linux_guest.sh
#
#  Then point NexOS at the result:
#    make play-linux   (VNC dev path)
#  or, on bare metal, the VMM backend attaches it as /system/linux_guest.img
# =====================================================================
set -euo pipefail

OUT="${OUT:-build/linux_guest.img}"
SIZE_MB="${SIZE_MB:-256}"
KERNEL="${NEXOS_LINUX_KERNEL:-}"
INITRD="${NEXOS_LINUX_INITRD:-}"
BUSYBOX_URL="${BUSYBOX_URL:-https://busybox.net/downloads/binaries/1.35.0-x86_64-linux-musl/busybox}"
CMDLINE="${CMDLINE:-console=ttyS0 root=/dev/sda rw}"

usage() {
    sed -n '2,45p' "$0" | sed 's/^# \{0,1\}//'
    exit 0
}

while [ $# -gt 0 ]; do
    case "$1" in
        -o|--out)    OUT="$2"; shift 2 ;;
        -s|--size)   SIZE_MB="$2"; shift 2 ;;
        -k|--kernel) KERNEL="$2"; shift 2 ;;
        -i|--initrd) INITRD="$2"; shift 2 ;;
        -h|--help)   usage ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

say() { printf '\033[1;36m[linux-guest]\033[0m %s\n' "$*"; }
die() { printf '\033[1;31m[linux-guest] %s\033[0m\n' "$*" >&2; exit 1; }

need() {
    command -v "$1" >/dev/null 2>&1 || die "missing tool: $1 ($2)"
}

# ---- prerequisites ---------------------------------------------------
need mkfs.ext2 "apt-get install e2fsprogs"
need mcopy     "apt-get install mtools"
need mdir      "apt-get install mtools"

# Kernel: prefer an explicit path, else the host's /boot.
if [ -z "$KERNEL" ]; then
    for k in /boot/vmlinuz /boot/vmlinuz-$(uname -r) /boot/bzImage; do
        [ -f "$k" ] && KERNEL="$k" && break
    done
fi
[ -n "$KERNEL" ] && [ -f "$KERNEL" ] || die "no Linux kernel found; pass -k /path/to/vmlinuz"
say "kernel : $KERNEL"

# ---- workspace -------------------------------------------------------
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/root"/{bin,sbin,etc,proc,sys,dev,tmp,root}

# ---- static busybox --------------------------------------------------
BB="$WORK/root/bin/busybox"
if command -v busybox >/dev/null 2>&1 && file "$(command -v busybox)" 2>/dev/null | grep -q "statically linked"; then
    cp "$(command -v busybox)" "$BB"
    say "busybox: host copy"
else
    say "busybox: downloading prebuilt static binary"
    if command -v curl >/dev/null 2>&1; then
        curl -fsSL "$BUSYBOX_URL" -o "$BB" || die "busybox download failed"
    elif command -v wget >/dev/null 2>&1; then
        wget -q "$BUSYBOX_URL" -O "$BB" || die "busybox download failed"
    else
        die "need curl or wget to fetch busybox"
    fi
    chmod +x "$BB"
fi
[ -s "$BB" ] || die "busybox is empty"

# ---- init (PID 1) ----------------------------------------------------
cat > "$WORK/root/init" <<'INIT'
#!/bin/busybox sh
/bin/busybox --install -s /bin
mount -t proc  none /proc 2>/dev/null
mount -t sysfs none /sys  2>/dev/null
mount -t devtmpfs none /dev 2>/dev/null
echo
echo "=============================================="
echo " NexOS Linux Guest - ready"
echo " framebuffer console on /dev/fb0"
echo "=============================================="
echo
# Hand the console to the shell so keystrokes from the NexOS window
# (delivered over the emulated vUART) reach it.
exec setsid /bin/sh
INIT
chmod +x "$WORK/root/init"

# ---- minimal /etc ----------------------------------------------------
cat > "$WORK/root/etc/inittab" <<'INITTAB'
::sysinit:/init
::askfirst:-/bin/sh
INITTAB

# ---- pack the raw image ---------------------------------------------
say "creating ${SIZE_MB} MiB image at $OUT"
mkdir -p "$(dirname "$OUT")"
rm -f "$OUT"
dd if=/dev/zero of="$OUT" bs=1M count="$SIZE_MB" status=none
mkfs.ext2 -q -F -L NEXOSLINUX "$OUT" >/dev/null

# mtools needs a drive letter pointing at a loop-less image offset.
MTOOLSRC="$(mktemp)"
trap 'rm -rf "$WORK" "$MTOOLSRC"' EXIT
echo "drive n: file=\"$OUT\" offset=0" > "$MTOOLSRC"
export MTOOLSRC

# Copy the tree in.  -s recurses; mtools creates directories as needed.
for d in bin etc root; do
    for f in "$WORK/root/$d"/*; do
        [ -e "$f" ] || continue
        mmd -i n: "::/$d" 2>/dev/null || true
    done
done
mmd -i n: ::/proc 2>/dev/null || true
mmd -i n: ::/sys  2>/dev/null || true
mmd -i n: ::/dev  2>/dev/null || true
mmd -i n: ::/tmp  2>/dev/null || true

mcopy -i n: -s "$WORK/root/bin/busybox" ::/bin/busybox
mcopy -i n:    "$WORK/root/init"        ::/init
mcopy -i n:    "$WORK/root/etc/inittab" ::/etc/inittab

# Place the kernel + optional initrd so the VMM loader can find them.
mcopy -i n: "$KERNEL" ::/vmlinuz
if [ -n "$INITRD" ] && [ -f "$INITRD" ]; then
    mcopy -i n: "$INITRD" ::/initrd.img
    say "initrd  : included"
fi

# Boot parameters for the VMM loader.
echo "$CMDLINE" > "$WORK/cmdline"
mcopy -i n: "$WORK/cmdline" ::/cmdline

# ---- report ----------------------------------------------------------
SECTORS=$(( SIZE_MB * 2048 ))
say "done: $OUT"
say "  size    : ${SIZE_MB} MiB (${SECTORS} sectors)"
say "  cmdline : $CMDLINE"
say ""
say "Attach it to the NexOS VMM as the guest block device, e.g.:"
say "  vmm_disk_attach(guest, \"/system/linux_guest.img\", $SECTORS);"
