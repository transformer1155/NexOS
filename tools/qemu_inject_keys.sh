#!/usr/bin/env bash
# Start QEMU with monitor on TCP and inject keystrokes to the QEMU monitor
# to type a command into the UEFI shell and press Enter.

ROOT="/mnt/d/MyOS/bootloader"
MONPORT=4444
QEMU_CMD="qemu-system-x86_64 -m 128 -machine q35 \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE.fd \
  -drive if=pflash,format=raw,file=$ROOT/build/ovmf_vars.fd \
  -drive file=$ROOT/build/os_uefi.img,format=raw,if=ide \
  -monitor tcp:127.0.0.1:${MONPORT},server,nowait \
  -serial file:$ROOT/build/serial.log -display none"

cd "$ROOT"
# Kill existing qemu
pkill -f qemu-system-x86_64 || true
sleep 1
# Start qemu in background
nohup bash -c "$QEMU_CMD" >/dev/null 2>&1 &
QPID=$!
echo "qemu pid=$QPID"

# Wait for monitor socket to be ready
for i in $(seq 1 30); do
  if nc -vz 127.0.0.1 ${MONPORT} >/dev/null 2>&1; then
	echo "monitor up"
	break
  fi
  sleep 1
done

# Build sendkey commands for the string
STR='FS0:\\\\EFI\\BOOT\\BOOTX64.EFI'
# map characters to sendkey tokens (best-effort)
declare -A map
for c in {a..z}; do map[$c]=$c; done
for c in {A..Z}; do map[$c]=${c,,}; done
map[0]=0; map[1]=1; map[2]=2; map[3]=3; map[4]=4; map[5]=5; map[6]=6; map[7]=7; map[8]=8; map[9]=9
map[:]=colon
map[\\]=backslash
map[/]=slash
map[ ]=space

# send each character via monitor sendkey
for ((i=0;i<${#STR};i++)); do
  ch="${STR:i:1}"
  token=${map[$ch]}
  if [ -z "$token" ]; then
	echo "no mapping for '$ch', skipping"
	continue
  fi
  printf "sendkey %s\n" "$token" | nc 127.0.0.1 ${MONPORT}
  sleep 0.05
done
# press Enter
printf "sendkey ret\n" | nc 127.0.0.1 ${MONPORT}

echo "injected command"

# Tail serial for a while
sleep 2
for i in $(seq 1 24); do
  echo "--- serial tail $i ---"
  tail -n 40 build/serial.log || true
  sleep 2
done

echo done
