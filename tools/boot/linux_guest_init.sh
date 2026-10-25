#!/bin/bash
# linux_guest_init.sh - runs inside QEMU#2 Alpine (self-contained bootstrap)
# Pre:  nothing — this script brings up networking + repos, then installs.
# Post: Xvfb + dbus + Chromium kiosk + x11vnc :5900

set -x
LOG=/tmp/orchestrator-init.log
exec >$LOG 2>&1

URL="${ORCH_URL:-https://example.com}"
DISPLAY=:0
VNC_PORT=5900
RESOLUTION="${RESOLUTION:-1024x768x24}"

# =====================================================================
# PERSISTENT ROOT PATH — if /dev/vda exists (guest_root.img ext4),
# skip apk install entirely and chroot into the prebuilt rootfs.
# =====================================================================
if [ -b /dev/vda ] && mkdir -p /mnt/disk && mount -t ext4 /dev/vda /mnt/disk 2>/dev/null; then
  echo "[orch-init] persistent root mounted on /mnt/disk"
  chroot /mnt/disk /usr/bin/Xvfb :0 -screen 0 $RESOLUTION -ac +extension RANDR >/tmp/xvfb.log 2>&1 &
  sleep 3
  chroot /mnt/disk /usr/bin/x11vnc -display :0 -rfbport $VNC_PORT -nopw -forever -shared -bg -o /tmp/x11vnc.log
  sleep 2
  echo "[orch-init] ALL UP (persistent root path)"
  exit 0
fi
echo "[orch-init] no /dev/vda or mount failed — falling back to apk path"

# Alpine version — prefer env override, else detect from guest release file, fallback v3.24.
if [ -n "$ALPINE_VER" ]; then
  :
elif [ -f /etc/alpine-release ]; then
  ALPINE_VER="v$(awk -F. '{print $1"."$2}' /etc/alpine-release)"
else
  ALPINE_VER="v3.24"
fi
echo "[orch-init] Alpine branch = $ALPINE_VER"

echo "[orch-init] starting at $(date)  URL=$URL  RES=$RESOLUTION  ALPINE=$ALPINE_VER"

# =====================================================================
# STEP 0 — NETWORK BOOTSTRAP (SLIRP DHCP + DNS + APK repos)
# =====================================================================
# The live ISO ships with ONLY the offline cdrom repo, no community, no network.
# This script must bring everything up itself to be fully self-contained.

echo "[orch-init] step 0: network bootstrap"

# 0a. DHCP on eth0 (SLIRP always names it eth0, gives us 10.0.2.16)
ip link set eth0 up 2>/dev/null || true
udhcpc -i eth0 -n -q 2>&1 | tee -a $LOG || true
sleep 2

# 0b. SLIRP DNS — gateway router on .3 of the guest subnet (QEMU default)
#     10.0.2.2 is the gateway, 10.0.2.3 is the built-in SLIRP DNS forwarder
printf 'nameserver 10.0.2.3\n' > /etc/resolv.conf
cat /etc/resolv.conf

# 0c. APK repos — overwrite the cdrom-only default.
#     HTTP (not HTTPS) to avoid CA-cert chicken-egg on a bare live ISO.
cat > /etc/apk/repositories <<EOF
http://dl-cdn.alpinelinux.org/alpine/${ALPINE_VER}/main
http://dl-cdn.alpinelinux.org/alpine/${ALPINE_VER}/community
EOF
cat /etc/apk/repositories

# 0d. apk update — refresh package list from online repos
echo "[orch-init] apk update"
apk update 2>&1 | tail -5

# =====================================================================
# STEP 1 — INSTALL DEPS (idempotent)
# =====================================================================
# "xvfb" lives in the community repo, requires step 0 above first.
# xvfb (community repo) provides /usr/bin/Xvfb directly; xorg-server also pulled in for completeness.
echo "[orch-init] step 1: apk add deps"
apk add --no-cache xvfb xorg-server x11vnc dbus dbus-x11 chromium ttf-dejavu font-noto-ansi font-noto-sans font-noto-cjk 2>&1 | tail -10

# =====================================================================
# STEP 2 — KILL STALE SURVIVORS
# =====================================================================
pkill -9 Xvfb x11vnc chromium 2>/dev/null || true; sleep 1

# =====================================================================
# STEP 3 — Xvfb virtual framebuffer
# =====================================================================
echo "[orch-init] step 3: Xvfb $DISPLAY -screen 0 $RESOLUTION"
Xvfb $DISPLAY -screen 0 $RESOLUTION -ac +extension RANDR &
XVFB_PID=$!
sleep 2
kill -0 $XVFB_PID 2>/dev/null || { echo "Xvfb FAILED"; exit 1; }
echo "[orch-init] Xvfb running pid=$XVFB_PID"

# =====================================================================
# STEP 4 — dbus session bus (Chromium requires it)
# =====================================================================
mkdir -p /tmp/dbus
DBUS_ADDR=$(dbus-daemon --fork --print-address 3 2>/dev/null || dbus-launch --sh-syntax | sed "s/DBUS_SESSION_BUS_ADDRESS=//" | tr -d "';")
export DBUS_SESSION_BUS_ADDRESS="${DBUS_ADDR:-unix:path=/run/dbus/system_bus_socket}"
echo "[orch-init] DBUS=$DBUS_SESSION_BUS_ADDRESS"

# =====================================================================
# STEP 5 — Chromium kiosk mode
# =====================================================================
rm -rf /tmp/chromium-profile; mkdir -p /tmp/chromium-profile
echo "[orch-init] step 5: chromium --kiosk --app=$URL"
DISPLAY=$DISPLAY DBUS_SESSION_BUS_ADDRESS="$DBUS_SESSION_BUS_ADDRESS" \
  chromium \
  --no-sandbox --disable-setuid-sandbox --disable-gpu \
  --kiosk --noerrdialogs --disable-infobars --disable-session-crashed-bubble \
  --disable-pinch --overscroll-behavior=none \
  --user-data-dir=/tmp/chromium-profile --app="$URL" &
CHROME_PID=$!
sleep 3
kill -0 $CHROME_PID 2>/dev/null || echo "WARNING: chromium died quickly"

# =====================================================================
# STEP 6 — x11vnc expose :0 on guest :5900
# =====================================================================
sleep 1
echo "[orch-init] step 6: x11vnc -display $DISPLAY -rfbport $VNC_PORT -nopw -forever"
x11vnc -display $DISPLAY -rfbport $VNC_PORT -nopw -forever &
VNC_PID=$!
sleep 1
echo "[orch-init] ALL UP: Xvfb=$XVFB_PID  Chromium=$CHROME_PID  x11vnc=$VNC_PID"

# Orchestrator polls host:5900 (hostfwd) — once x11vnc binds, _wait_vnc_up succeeds.