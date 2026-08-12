#!/usr/bin/env bash
# Start QEMU in background and poll serial.log for kernel markers
set -e
ROOT="/mnt/d/MyOS/bootloader"
cd "$ROOT"
# Ensure previous qemu killed
pkill -f qemu || true
sleep 1

# Start QEMU
nohup qemu-system-x86_64 \
  -m 128 -machine q35 \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE.fd \
  -drive if=pflash,format=raw,file=build/ovmf_vars.fd \
  -drive file=build/os_uefi.img,format=raw,if=ide \
  -serial file:build/serial.log -display none >/dev/null 2>&1 &
QEMU_PID=$!
echo $QEMU_PID > /tmp/qemu.pid

echo "started qemu pid $QEMU_PID"

MAX_ITER=36
SLEEP=5
for i in $(seq 1 $MAX_ITER); do
  sleep $SLEEP
  echo "---- poll $i ----"
  if grep -a "\[K1\]" build/serial.log >/dev/null 2>&1; then
	echo "FOUND [K1] in serial.log"
	break
  fi
  if [ -f build/serial.log ]; then
	echo "--- tail serial.log ---"
	tail -n 40 build/serial.log || true
  else
	echo "serial.log not found yet"
  fi
done

echo "done polling"
# keep qemu running; user can kill it later
exit 0
