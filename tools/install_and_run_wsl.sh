#!/bin/bash
set -e

# Install dependencies (Ubuntu WSL/WSL2)
sudo apt update
sudo apt install -y build-essential gcc-multilib g++-multilib binutils nasm qemu-system-x86 ovmf gnu-efi python3 python3-pip pkg-config

# Copy repository to WSL home for faster I/O (optional)
if [ -d "/mnt/d/MyOS/bootloader" ]; then
  echo "Found Windows repo at /mnt/d/MyOS/bootloader"
  echo "Copying to ~/bootloader for faster build (optional, can skip)"
  cp -r /mnt/d/MyOS/bootloader ~/bootloader || true
  cd ~/bootloader
else
  echo "Please git clone or copy your repo into WSL and run this script from the repo root"
  exit 1
fi

# Ensure build dir exists
mkdir -p build

# Build UEFI image
make uefi -j$(nproc)

# Run QEMU and capture serial output to serial.log
# Adjust OVMF path if necessary
OVMF_CODE=/usr/share/OVMF/OVMF_CODE.fd
if [ ! -f "$OVMF_CODE" ]; then
  echo "OVMF firmware not found at $OVMF_CODE - try locate /usr/share/OVMF or install ovmf package"
fi

echo "Starting QEMU - serial output will be shown and saved to build/serial.log"
qemu-system-x86_64 -serial file:build/serial.log -drive format=raw,file=build/os_uefi.img -m 2048 -bios $OVMF_CODE &
QEMU_PID=$!

# Wait for boot to proceed (user can Ctrl-C to stop)
sleep 2

echo "Streaming serial.log (press Ctrl-C to stop streaming)"
tail -f build/serial.log

# On exit, kill qemu
kill $QEMU_PID 2>/dev/null || true

