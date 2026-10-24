# NexOS Linux Guest — Networking Topology

> All networking built on QEMU user-net (SLIRP).  Host cannot initiate
> UDP/TCP into guests; guest->host always works.

## 1. Three Fixed Channels

| Channel | NexOS side | Host side | QEMU#2 guest side | Port |
|---------|-----------|-----------|-------------------|------|
| VNC | vnc_client -> TCP | orchestrator proxy | x11vnc | 5900 (guest) -> 15900 (host) |
| Serial Ctrl | COM2 0x2F8 -> linux_manager | orchestrator _ctrl_acceptor | -- | 15901 |
| Init Push | -- | orchestrator _init_push_acceptor | /dev/ttyS1 | 15902 |

## 2. IP Map

HOST localhost 127.0.0.1
  orchestrator.py listens:
    0.0.0.0:15900  <- NexOS VNC client
    127.0.0.1:15901 <- QEMU#1 COM2 ctrl
    127.0.0.1:15902 <- QEMU#2 COM2 init
  QEMU#2 hostfwd:
    tcp::5900-:5900  (guest x11vnc)
    tcp::15902-:15902 (init passthrough)

QEMU#1 (NexOS):  10.0.2.15/24 SLIRP, gw=10.0.2.2=host
  COM1 0x3F8 -> -serial mon:stdio
  COM2 0x2F8 -> -serial tcp:127.0.0.1:15901
  linux_manager.cpp writes COM2 "start\n"/"stop\n"
  vnc_client.cpp connects TCP 10.0.2.2:15900

QEMU#2 (Alpine): 10.0.2.16/24 SLIRP (separate user-net)
  -net nic,model=virtio-net-pci
  -net user,hostfwd=tcp::5900-:5900,hostfwd=tcp::15902-:15902
  -serial mon:stdio
  -serial tcp:127.0.0.1:15902  (client mode)
  After init: Xvfb :0 + Chromium kiosk + x11vnc :5900

## 3. QEMU#1 NexOS Cmdline

  qemu-system-x86_64 \
    -drive format=raw,file=build/os_v2.img \
    -m 4096 -cpu EPYC \
    -net nic,model=virtio-net-pci -net user \
    -vga std -display sdl \
    -serial mon:stdio \
    -serial tcp:127.0.0.1:15901 \
    -no-reboot

## 4. QEMU#2 Alpine Cmdline

  qemu-system-x86_64 \
    -name NexOS-Linux -cpu qemu64 -smp 4 -m 2048 \
    -vga std \
    -net nic,model=virtio-net-pci \
    -net user,hostfwd=tcp::5900-:5900,hostfwd=tcp::15902-:15902 \
    -serial mon:stdio \
    -serial tcp:127.0.0.1:15902 \
    -display sdl -no-reboot \
    -drive format=qcow2,file=alpine.qcow2,if=virtio

## 5. SLIRP Hard Rule

Guest->host TCP ALWAYS works (gateway 10.0.2.2).
Host->guest TCP BLOCKED without hostfwd.

Therefore VNC goes HOST PROXY path:
  NexOS vnc_client -> 10.0.2.2:15900 -> orchestrator proxy -> host:5900 -> QEMU#2 hostfwd -> guest:5900
Serial ctrl: QEMU#1 COM2 client serial connects to host listener (guest->host, ok).
Init push: QEMU#2 COM2 client serial connects to host listener (guest->host, ok).

## 6. Boot Sequence

1. QEMU#2 boots Alpine
2. /etc/local.d/00-orchestrator.start opens /dev/ttyS1
3. Orchestrator detects QEMU#2 serial connect, pushes base64 init script
4. Guest decodes + execs -> Xvfb -> Chromium -> x11vnc :5900
5. Orchestrator _wait_vnc_up sees :5900 open, sends "ready"
6. NexOS linux_manager opens vnc_client, renders into window