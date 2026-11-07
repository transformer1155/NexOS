#!/usr/bin/env bash
# =====================================================================
#  qemu_dos_test.sh  -  boot NexOS in QEMU and drive `dos` from the shell
#
#  Runs the real build/os_v2.img on qemu-system-i386 with the serial port
#  redirected to stdio, types the `dos` commands into the shell, and greps
#  the transcript for the expected output.
#
#  Usage:
#     bash qemu_dos_test.sh              # headless, automated checks
#     bash qemu_dos_test.sh --gui        # interactive window, for eyeballing
# =====================================================================
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NEXROOT="$(cd "$HERE/.." && pwd)"
IMG="$NEXROOT/build/os_v2.img"

MODE="${1:-auto}"

if [ ! -f "$IMG" ]; then
  echo "!! $IMG not built yet -- run: make"
  exit 1
fi

if [ "$MODE" = "--gui" ]; then
  echo "==> interactive QEMU (close the window when done)"
  exec qemu-system-i386 -m 256 -drive file="$IMG",format=raw -serial stdio
fi

echo "==> headless QEMU boot + DOS command test"
echo "    image: $IMG"

# Boot a COPY with -snapshot semantics: the real image is never locked or
# modified, so a concurrent QEMU/VBox holding it cannot block the test.
RUNIMG="$HERE/out/os_dos_test.img"
cp -f "$IMG" "$RUNIMG" 2>/dev/null || {
  echo "!! could not copy image (in use?)"
  exit 1
}

# Feed commands after a delay so the kernel finishes booting.  Each line is
# sent with a short pause; qemu reads stdin as keystrokes on the serial line.
{
  sleep 14
  printf 'root\n'                    # login
  sleep 3
  printf 'admin\n'                   # password
  sleep 4
  printf 'probe minprobe\n'
  sleep 6
  printf 'probe userdemo\n'
  sleep 6
} | timeout 130 qemu-system-x86_64 \
      -m 128M \
      -drive file="$RUNIMG",format=raw \
      -vga none \
      -display none \
      -serial stdio \
      -no-reboot > "$HERE/out/qemu_dos.log" 2>&1

echo
echo "==> transcript (probe lines)"
grep -aE 'probe|MINPROBE|RING3|userdemo|EXCEPTION|#GP|#PF' \
     "$HERE/out/qemu_dos.log" | head -40

echo
rc=0
for pat in 'MINPROBE_OK' 'RETURNED'; do
  if grep -qa "$pat" "$HERE/out/qemu_dos.log"; then
    echo "ok  : found '$pat'"
  else
    echo "MISS: '$pat'"
    rc=1
  fi
done
echo
echo "==> overall: $([ $rc -eq 0 ] && echo PASS || echo FAIL)"
echo "    full log: $HERE/out/qemu_dos.log"
exit $rc