#!/usr/bin/env bash
set -uo pipefail
LOG=/tmp/smoke_all.log
exec >"$LOG" 2>&1
echo "[smoke] start $(date)"
pkill -f "qemu-system" 2>/dev/null
pkill -f "Xvfb" 2>/dev/null
sleep 1
Xvfb :3 -screen 0 1024x768x24 >/tmp/xvfb3.log 2>&1 &
XVFB_PID=$!
sleep 1
echo "[smoke] Xvfb pid=$XVFB_PID"
DISPLAY=:3 qemu-system-x86_64 -drive format=raw,file=build/os_v2.img -m 64M \
  -display sdl -vga std -no-reboot -monitor none \
  -serial tcp:127.0.0.1:9997,server,nowait \
  -net nic,model=ne2k_isa -net user,hostfwd=tcp::8080-:8080 \
  -device loader,addr=0x501F,data=1,data-len=1 \
  >/tmp/qemu_smoke.log 2>&1 &
QEMU_PID=$!
echo "[smoke] QEMU pid=$QEMU_PID"
sleep 30
echo "[smoke] grabbing frame"
ffmpeg -y -f x11grab -s 1024x768 -i :3.0 -frames:v 1 /tmp/boot_frame.png >/tmp/ff.log 2>&1 && echo "[smoke] frame saved" || echo "[smoke] grab failed"
ls -la /tmp/boot_frame.png 2>&1
echo "[smoke] qemu log tail:"; tail -10 /tmp/qemu_smoke.log 2>&1
kill -9 $QEMU_PID 2>/dev/null; kill -9 $XVFB_PID 2>/dev/null
echo "[smoke] done $(date)"
