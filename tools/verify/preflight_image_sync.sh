#!/bin/bash
# ============================================================================
#  preflight_image_sync.sh - is build/os_v2.img actually built from the CURRENT
#  build/kernel.bin, build/kernel64.bin and build/sfs.img?
# ----------------------------------------------------------------------------
#  WHY THIS EXISTS
#    A stale image is invisible in the logs: the machine boots, the GUI appears,
#    and every conclusion is drawn against an older kernel. That happened for
#    real -- os_v2.img embedded a kernel64.bin predating the current one, so a
#    64-bit investigation was reading the wrong kernel.
#
#    mtime is NOT sufficient evidence: 'image newer than its input' is true of
#    every healthy image and proves nothing. To claim 'the image contains this
#    kernel' you must compare the EMBEDDED BYTES. That is all this script does.
#
#  BOUNDARY (important - do not over-read a pass)
#    It proves:   build/os_v2.img == build/{kernel,kernel64}.bin + build/sfs.img
#    It does NOT:      build/*.bin == the latest SOURCE
#    If someone edited a .cpp without rebuilding, the image is genuinely in sync
#    with the products and this reports IN SYNC -- correctly, but that says
#    nothing about the newest source. Use `make -q <target>` for that question.
#
#  LAYOUT (read out of the Makefile recipe, not assumed):
#    $(IMG): cat boot.bin stage2.bin kernel.bin > img ; truncate -s 4M
#            dd kernel64.bin bs=512 seek=2048  -> offset 1048576
#            dd sfs.img      bs=512 seek=6144  -> offset 3145728
#    kernel.bin's offset is therefore COMPUTED (boot.bin+stage2.bin), not fixed.
#
#  EXIT CODES (same convention as tools/verify/preflight_idle.sh)
#    0 = in sync          (safe to draw conclusions from this image)
#    1 = stale / broken   (a mismatch WAS found - the image is definitely wrong)
#    3 = cannot verify    (missing tools or inputs, or a build is in flight)
#  rc=3 must never be treated as a pass: 'could not check' is not 'checked OK'.
#
#  WARNING -- do not read the exit code through a pipe:
#    bash preflight_image_sync.sh | tail -8; echo rc=$?   # prints TAIL's rc (0),
#    even though this script printed 'RESULT: IMAGE STALE OR BROKEN' and exited 1.
#    A pipeline makes $? the LAST command's status, so the visible first line and
#    the rc disagree -- and a stale image ends up looking 'in sync'.
#    Correct ways to keep both:
#      bash preflight_image_sync.sh > /tmp/pf.log 2>&1; rc=$?     # then read the log
#      bash preflight_image_sync.sh | tail -8; rc=${PIPESTATUS[0]}   # bash only
#    This bit two people on the same day.  The whole point of rc=1 vs rc=3 vs rc=0
#    is that 'stale' and 'cannot verify' must never be read as 'ok'; a pipe
#    silently reintroduces exactly that mistake.
#
#  Pure read-only: starts nothing, builds nothing, changes no state.
#  IMG=<path> and REPO_ROOT=<path> override for testing / other images.
# ============================================================================
set -u
root=${REPO_ROOT:-}
if [ -z "$root" ]; then
  root=$(cd "$(dirname "$0")" && pwd)
  while [ "$root" != "/" ] && [ ! -f "$root/Makefile" ]; do root=$(dirname "$root"); done
fi
[ -f "$root/Makefile" ] || { echo "cannot verify: no Makefile at $root (set REPO_ROOT)"; exit 3; }
cd "$root" || exit 3
IMG=${IMG:-build/os_v2.img}

# --- tools present?  Missing tool => cannot verify, NOT stale ----------------
missing=""
for t in dd cmp stat sleep; do
  command -v "$t" >/dev/null 2>&1 || missing="$missing $t"
done
if [ -n "$missing" ]; then
  echo "cannot verify: missing tool(s):$missing"
  exit 3
fi

# --- inputs present? --------------------------------------------------------
absent=""
for f in "$IMG" build/kernel.bin build/kernel64.bin build/sfs.img build/boot.bin build/stage2.bin sfs_files/shell.mex; do
  [ -e "$f" ] || absent="$absent $f"
done
if [ -n "$absent" ]; then
  echo "cannot verify: missing input(s):$absent"
  exit 3
fi

# --- a build in flight?  Then no byte comparison is meaningful ---------------
A=$(stat -c%s "$IMG"); sleep 2; B=$(stat -c%s "$IMG")
if [ "$A" != "$B" ]; then
  echo "cannot verify: image size is changing ($A -> $B) - a build is writing it"
  exit 3
fi

bad=0

# --- 1) packing order: the SFS must not predate its own input ----------------
if [ build/sfs.img -ot sfs_files/shell.mex ]; then
  echo "  STALE    shell.mex is NEWER than build/sfs.img (SFS packed before its input)"; bad=1
else
  echo "  ok       shell.mex <= sfs.img"
fi

# --- 2) embedded SFS at LBA 6144 --------------------------------------------
SZ=$(stat -c%s build/sfs.img); SEC=$(( (SZ + 511) / 512 ))
if dd if="$IMG" bs=512 skip=6144 count=$SEC 2>/dev/null | head -c "$SZ" | cmp -s - build/sfs.img; then
  echo "  ok       SFS_EMBED_MATCH        ($SZ B at LBA 6144)"
else
  echo "  STALE    SFS_EMBED_DIFFERS      (LBA 6144 != build/sfs.img)"; bad=1
fi

# --- 3) embedded 64-bit kernel at LBA 2048 ----------------------------------
# Block read + exact truncation, the same shape the SFS branch above uses.
# These two branches used to read with bs=1: 1,035,248 and 588,604 single-byte
# read() calls.  On vboxsf that is ~1.6M round trips and the script never
# finished -- three timeouts and one stuck take-8 before anyone spotted it.
# A checker that cannot complete is not a checker, so: bs=512 + head -c.
#
# KNOWN MISCLASSIFICATION (recorded, deliberately not fixed): if dd cannot read the
# image at all, head -c yields an empty stream and cmp reports DIFFERS, so the script
# returns rc=1 (stale) where rc=3 (cannot verify) is the correct semantics.  The
# direction is SAFE -- a false failure, never a false pass -- and changing it would
# mean re-verifying a tool with no misjudgement history.  Recorded so that anyone who
# sees 'unreadable image reported as STALE' blames this decision, not cmp.
K64=$(stat -c%s build/kernel64.bin); K64SEC=$(( (K64 + 511) / 512 ))
if dd if="$IMG" bs=512 skip=2048 count=$K64SEC 2>/dev/null | head -c "$K64" | cmp -s - build/kernel64.bin; then
  echo "  ok       KERNEL64_EMBED_MATCH   ($K64 B at LBA 2048)"
else
  echo "  STALE    KERNEL64_EMBED_DIFFERS (LBA 2048 != build/kernel64.bin)  <-- 64-bit results invalid"; bad=1
fi

# --- 4) embedded 32-bit kernel, right after boot.bin + stage2.bin ------------
# Same block-read shape.  OFF is computed, so assert it is sector aligned before
# dividing: if boot.bin/stage2.bin ever grow to a non-multiple of 512, a silent
# skip=OFF/512 would read the WRONG range and still compare cleanly against some
# other region -- a false picture of sync.  Refuse instead (cannot verify, rc=3,
# which is the documented code for 'could not check'; NOT a 4th code, so the
# 0/1/3 contract shared with preflight_idle.sh stays intact).
OFF=$(( $(stat -c%s build/boot.bin) + $(stat -c%s build/stage2.bin) ))
if [ $(( OFF % 512 )) -ne 0 ]; then
  echo "cannot verify: boot.bin+stage2.bin = $OFF B is not 512-aligned (internal invariant)"
  echo "                refusing to divide it into a sector number"
  exit 3
fi
K32=$(stat -c%s build/kernel.bin); K32SEC=$(( (K32 + 511) / 512 ))
if dd if="$IMG" bs=512 skip=$(( OFF / 512 )) count=$K32SEC 2>/dev/null | head -c "$K32" | cmp -s - build/kernel.bin; then
  echo "  ok       KERNEL32_EMBED_MATCH   ($K32 B at offset $OFF)"
else
  echo "  STALE    KERNEL32_EMBED_DIFFERS (offset $OFF)"; bad=1
fi

echo "  ok       image stable at $A bytes"

# --- timestamp comparison: WHY it differs, in human terms -------------------
# DIFFERS tells you the bytes disagree; it does not tell you why.  The common
# cause is simply that a kernel was rebuilt after the image was emitted
# ("forgot to re-emit"), and mtimes alone show that -- a two-hour gap should
# raise suspicion before anyone bothers with a byte comparison.  Printed on
# every run, healthy or not, so the margin is visible either way.
img_t=$(stat -c%Y "$IMG")
hms() { date -d "@$1" +%H:%M:%S; }
age_str() {                     # $1 = input epoch, $2 = image epoch
  d=$(( $1 - $2 ))
  dir="OLDER"
  if [ "$d" -lt 0 ]; then d=$(( -d )); dir="NEWER"; fi
  # Three scales, chosen PER LINE (one input can differ by seconds while another
  # is hours out).  Measured: the healthy state is seconds, which is the common
  # case; minutes/hours appear when something was rebuilt after the image was
  # emitted; days when the image is a forgotten artifact.
  # '192h00m' is arithmetically right but unreadable -> days past 24h;
  # '0h00m' hides whether it was 5 s or 55 s -> seconds under a minute.
  if [ "$d" -ge 86400 ]; then
    printf '%dd%02dh %s' $(( d / 86400 )) $(( (d % 86400) / 3600 )) "$dir"
  elif [ "$d" -ge 60 ]; then
    printf '%dh%02dm %s' $(( d / 3600 )) $(( (d % 3600) / 60 )) "$dir"
  else
    # A healthy image is emitted seconds after its kernels, so the COMMON case
    # is sub-minute: measured output showed '0h00m NEWER', which hides whether
    # that was 5 s or 55 s.  Report seconds there.
    printf '%ds %s' "$d" "$dir"
  fi
}
echo "  -- timestamps: image vs its inputs --"
for f in build/kernel.bin build/kernel64.bin build/sfs.img; do
  [ -e "$f" ] || continue
  t=$(stat -c%Y "$f")
  printf '     %-22s mtime=%s   image=%s   -> image is %s\n' \
    "$f" "$(hms "$t")" "$(hms "$img_t")" "$(age_str "$t" "$img_t")"
done

if [ "$bad" = 0 ]; then echo "RESULT: IMAGE IN SYNC"; exit 0; fi
echo "RESULT: IMAGE STALE OR BROKEN (do not draw conclusions from it)"; exit 1
