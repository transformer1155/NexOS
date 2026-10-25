#!/usr/bin/env bash
# build_full_guest.sh — 构建完整 Linux 访客 initrd
#   busybox + Xvfb + x11vnc + Mesa/llvmpipe(OpenGL 4.5) + Unity 游戏
#
# 产物:
#   /tmp/guestroot/                           访客根文件系统
#   linux_root/guest-initramfs.cpio.gz        最终 initrd
#   linux_root/vmlinuz-guest                  拷贝自 /boot/vmlinuz-6.8.0-146-generic
#
# QEMU 启动:
#   qemu-system-x86_64 -m 2048 -smp 2 \
#     -kernel linux_root/vmlinuz-guest \
#     -initrd linux_root/guest-initramfs.cpio.gz \
#     -append "console=ttyS0 panic=-1 net.ifnames=0" \
#     -netdev user,id=n0,hostfwd=tcp::15902-:5900 \
#     -device virtio-net-pci,netdev=n0 \
#     -display none -serial stdio

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="${ROOT:-/tmp/guestroot}"
OUT="${OUT:-$SCRIPT_DIR/../linux_root/guest-initramfs.cpio.gz}"
VMlinuz_OUT="${VMlinuz_OUT:-$SCRIPT_DIR/../linux_root/vmlinuz-guest}"
GAME_URL="${GAME_URL:-}"  # 可选: 直接指定 Unity 游戏下载地址

echo "=============================================="
echo " Full Linux Guest Builder for NexOS VNC Mirror"
echo " ROOT=$ROOT"
echo " OUT=$OUT"
echo "=============================================="

# ---------- 0. 清理 ----------
echo "[0/8] 清理旧 rootfs"
rm -rf "$ROOT"
mkdir -p "$ROOT"/{bin,sbin,etc,proc,sys,dev,tmp,var,run,lib,lib64,x11,usr/bin,usr/sbin,usr/libexec,usr/share}

# ---------- 1. busybox ----------
echo "[1/8] busybox + applets"
cp /usr/bin/busybox "$ROOT/bin/busybox"
cp -a -L /usr/bin/busybox "$ROOT/init"   # PID 1 必须名 "init"
for app in sh mount sleep echo cat grep setsid mknod ifconfig udhcpc \
           mkdir ln ls ps kill rm awk sed head tail wc tr cut tee sort uniq \
           env printenv id chmod chown chpst true false yes no nohup nice \
           tar gzip gunzip zcat dd df du free uname hostname stty \
           md5sum sha1sum sha256sum hexdump od strings; do
  ln -sf busybox "$ROOT/bin/$app"
done

# ---------- 2. 关键二进制 ----------
echo "[2/8] 关键二进制 (Xvfb/x11vnc/Mesa/curl)"
BIN_SRC=(
  /usr/bin/Xvfb /usr/bin/x11vnc /usr/bin/xvfb-run
  /usr/bin/glxgears /usr/bin/glxinfo
  /usr/sbin/ip
  /usr/bin/xkbcomp /usr/bin/xmodmap /usr/bin/xrdb /usr/bin/xset /usr/bin/xsetroot
  /usr/bin/xterm
  /usr/bin/curl /usr/bin/wget
  /usr/bin/basename /usr/bin/dirname /usr/bin/head /usr/bin/tail
  /usr/bin/tee /usr/bin/sleep /usr/bin/timeout /usr/bin/which
  /usr/bin/id /usr/bin/ps /usr/bin/netstat /usr/bin/ping
  /usr/bin/kmod /sbin/modprobe /sbin/modinfo /sbin/insmod /sbin/rmmod /sbin/lsmod
  /usr/bin/udevadm /lib/systemd/systemd-udevd
  /usr/bin/dbus-daemon /usr/bin/dbus-run-session
)
for b in "${BIN_SRC[@]}"; do
  [ -f "$b" ] && cp -a -L "$b" "$ROOT/usr/bin/" || true
done
# Special paths
[ -f /usr/sbin/ip ]         && cp -a -L /usr/sbin/ip         "$ROOT/sbin/"
[ -f /usr/bin/ip ]          && cp -a -L /usr/bin/ip          "$ROOT/sbin/ip"
# curl CA bundle
[ -f /etc/ssl/certs/ca-certificates.crt ] && cp -a /etc/ssl/certs "$ROOT/etc/ssl/"
[ -f /etc/resolv.conf ]    && cp -a /etc/resolv.conf        "$ROOT/etc/"

# ---------- 3. 递归拷贝共享库 ----------
echo "[3/8] 递归拷贝所有 .so (ldd + dlopen 补漏 + DRI)"
LIBD="$ROOT/lib/x86_64-linux-gnu"
mkdir -p "$LIBD"

copy_deps_recursive() {
  local bin="$1"
  [ -f "$bin" ] || return
  ldd "$bin" 2>/dev/null | awk '{print $3}' | grep '^/' | sort -u | while read -r lib; do
    [ -z "$lib" ] && continue
    local real
    real=$(readlink -f "$lib" 2>/dev/null) || continue
    [ -z "$real" ] && continue
    local target="$LIBD/$(basename "$real")"
    [ -f "$target" ] && continue
    cp -a "$real" "$target"
    copy_deps_recursive "$real"
  done
}

for b in "$ROOT"/bin/* "$ROOT"/sbin/* "$ROOT"/usr/bin/*; do
  [ -x "$b" ] && copy_deps_recursive "$b" 2>/dev/null || true
done
# 宿主系统工具（不在 ROOT 里但需要）
for b in /usr/bin/glxgears /usr/bin/glxinfo /usr/lib/x86_64-linux-gnu/dri/swrast_dri.so \
         /usr/lib/x86_64-linux-gnu/dri/kms_swrast_dri.so; do
  copy_deps_recursive "$b" 2>/dev/null || true
done

# 强制拷贝所有 DRI 驱动（Unity 可能探测多种）
DRI_DIR="$ROOT/usr/lib/x86_64-linux-gnu/dri"
mkdir -p "$DRI_DIR"
for dri in /usr/lib/x86_64-linux-gnu/dri/*.so; do
  cp -a "$dri" "$DRI_DIR/"
done
# DRI 依赖的 libdrm
cp -a -L /lib/x86_64-linux-gnu/libdrm.so.2 "$LIBD/" 2>/dev/null || true
cp -a -L /lib/x86_64-linux-gnu/libgbm.so.1 "$LIBD/" 2>/dev/null || true

# dlopen 补漏 (Unity/Mesa)
for pat in \
    libXdamage.so.1 libXfixes.so.3 libXxf86vm.so.1 libXpresent.so.1 \
    libxshmfence.so.1 libxcb-dri3.so.0 libxcb-present.so.0 libxcb-sync.so.1 \
    libxcb-xfixes.so.0 libxcb-randr.so.0 libxcb-shape.so.0 libxcb-shm.so.0 \
    libxcb-glx.so.0 libxcb-icccm.so.4 libxcb-image.so.0 libxcb-keysyms.so.1 \
    libxcb-render-util.so.0 libxcb-util.so.1 \
    libGL.so.1 libEGL.so.1 libGLESv1_CM.so.1 libGLESv2.so.2 libGLX.so.0 \
    libGLdispatch.so.0 libGLX_mesa.so.0 libOSMesa.so.8 \
    libgallium-2504.so libgallium-2404.so libgallium-2206.so \
    libLLVM.so.18 libLLVM.so.20 libLLVM-18.so libLLVM-20.so \
    libasound.so.2 libpulse.so.0 libpulse-mainloop-glib.so.0 \
    libdbus-1.so.3 libexpat.so.1 libpng16.so.16 libjpeg.so.8 libtiff.so.6 \
    libwebp.so.7 libwebpmux.so.3 libglib-2.0.so.0 libudev.so.1 \
    liblz4.so.1 libzstd.so.1 libsnappy.so.1 libbz2.so.1 libgcrypt.so.20 \
    libgpg-error.so.0 libgdk_pixbuf-2.0.so.0 libgtk-3.so.0 libgobject-2.0.so.0 \
    libfontconfig.so.1 libfreetype.so.6 libharfbuzz.so.0 libgraphite2.so.3 \
    libicui18n.so.74 libicuuc.so.74 libicudata.so.74 libdbus-glib-1.so.2 \
    libcap.so.2 libattr.so.1 libacl.so.1 libselinux.so.1 libpcre2-8.so.0 \
    libpcre.so.3 libffi.so.8 libmount.so.1 libblkid.so.1 libuuid.so.1 \
    libz.so.1 libcrypto.so.3 libssl.so.3 libcurl.so.4; do
  HOST=$(find /lib /usr/lib -name "$pat" -type f 2>/dev/null | head -n 1)
  if [ -n "$HOST" ]; then
    real=$(readlink -f "$HOST" 2>/dev/null) || real="$HOST"
    cp -a "$real" "$LIBD/" 2>/dev/null || true
  fi
done

# ---------- 4. 重建 soname symlinks ----------
echo "[4/8] 重建 soname symlinks"
cd "$LIBD" || exit 1
# 处理 lib*.so.X.Y.Z -> lib*.so.X -> lib*.so
find . -maxdepth 1 -name '*.so*' -type f | while read -r real; do
  bn=$(basename "$real")
  # 形如 libfoo.so.1.2.3
  if [[ "$bn" =~ ^(.*\.so)\.([0-9]+)\.[0-9]+\.[0-9]+$ ]]; then
    soname="${BASH_REMATCH[1]}.${BASH_REMATCH[2]}"
    [ ! -e "$soname" ] && ln -sf "$bn" "$soname"
    major="${BASH_REMATCH[1]}"
    [ ! -e "$major" ] && ln -sf "$soname" "$major"
  elif [[ "$bn" =~ ^(.*\.so)\.([0-9]+)\.[0-9]+$ ]]; then
    soname="${BASH_REMATCH[1]}.${BASH_REMATCH[2]}"
    [ ! -e "$soname" ] && ln -sf "$bn" "$soname"
    major="${BASH_REMATCH[1]}"
    [ ! -e "$major" ] && ln -sf "$soname" "$major"
  elif [[ "$bn" =~ ^(.*\.so)\.([0-9]+)$ ]]; then
    major="${BASH_REMATCH[1]}"
    [ ! -e "$major" ] && ln -sf "$bn" "$major"
  fi
done
cd - >/dev/null

# ---------- 5. 动态链接器 + dev + etc + 数据文件 ----------
echo "[5/8] 动态链接器 /dev /etc /usr/share"
LOADER_HOST=$(readlink -f /lib64/ld-linux-x86-64.so.2 2>/dev/null ||
              find /lib /usr/lib -name 'ld-linux-x86-64.so*' -type f 2>/dev/null | head -n 1)
if [ -n "$LOADER_HOST" ]; then
  mkdir -p "$ROOT/lib64"
  cp -a "$LOADER_HOST" "$ROOT/lib64/"
fi

# /dev
mknod -m 666 "$ROOT/dev/null"    c 1 3   2>/dev/null
mknod -m 666 "$ROOT/dev/zero"    c 1 5   2>/dev/null
mknod -m 666 "$ROOT/dev/random"  c 1 8   2>/dev/null
mknod -m 666 "$ROOT/dev/urandom" c 1 9   2>/dev/null
mknod -m 600 "$ROOT/dev/console" c 5 1   2>/dev/null
mknod -m 600 "$ROOT/dev/ttyS0"   c 4 64  2>/dev/null
mkdir -p "$ROOT/dev/pts" "$ROOT/dev/shm"

# /etc
cat > "$ROOT/etc/ld.so.conf" <<'EOF'
/lib
/usr/lib
/lib/x86_64-linux-gnu
/usr/lib/x86_64-linux-gnu
EOF
cat > "$ROOT/etc/passwd" <<'EOF'
root:x:0:0:root:/root:/bin/sh
EOF
cat > "$ROOT/etc/group" <<'EOF'
root:x:0:
EOF

# XKB 数据 (Unity 启动需要)
if [ -d /usr/share/X11/xkb ]; then
  cp -a /usr/share/X11/xkb "$ROOT/usr/share/X11/"
fi

# fontconfig 配置
mkdir -p "$ROOT/etc/fonts"
if [ -f /etc/fonts/fonts.conf ]; then
  cp -a /etc/fonts/fonts.conf "$ROOT/etc/fonts/"
fi
if [ -d /etc/fonts/conf.d ]; then
  cp -a /etc/fonts/conf.d "$ROOT/etc/fonts/"
fi
# 至少一个字体（Unity 必需）
mkdir -p "$ROOT/usr/share/fonts/truetype/dejavu"
if [ -f /usr/share/fonts/truetype/dejavu/DejaVuSans.ttf ]; then
  cp -a /usr/share/fonts/truetype/dejavu/*.ttf "$ROOT/usr/share/fonts/truetype/dejavu/"
fi
# fc-cache 二进制
cp -a -L /usr/bin/fc-cache "$ROOT/usr/bin/" 2>/dev/null || true
copy_deps_recursive /usr/bin/fc-cache 2>/dev/null || true

# D-Bus
mkdir -p "$ROOT/run/dbus" "$ROOT/etc/dbus-1"
if [ -d /usr/share/dbus-1 ]; then
  cp -a /usr/share/dbus-1 "$ROOT/usr/share/"
fi
if [ -d /etc/dbus-1 ]; then
  cp -a /etc/dbus-1 "$ROOT/etc/"
fi

# udev rules (X 可能探测)
mkdir -p "$ROOT/lib/udev/rules.d"
if [ -d /lib/udev/rules.d ]; then
  cp -a /lib/udev/rules.d/50-udev-default.rules "$ROOT/lib/udev/rules.d/" 2>/dev/null || true
fi

# ---------- 6. 可选: Unity 游戏 ----------
echo "[6/8] Unity 游戏 (如指定 GAME_URL 则下载)"
GAME_DIR="$ROOT/opt/game"
mkdir -p "$GAME_DIR"
if [ -n "$GAME_URL" ]; then
  echo "  下载 $GAME_URL ..."
  cd /tmp
  curl -L -o game.tgz "$GAME_URL"
  tar xzf game.tgz -C "$GAME_DIR" --strip-components=1
  chmod +x "$GAME_DIR"/*.x86_64 2>/dev/null || true
  cd -
else
  echo "  未指定 GAME_URL, 跳过。guest 里可用 glxgears 验证渲染链路。"
  # 创建一个启动占位脚本
  cat > "$GAME_DIR/run.sh" <<'EOF'
#!/bin/sh
# 从 guest 内运行: /opt/game/run.sh
echo "运行 glxgears 验证 Mesa/llvmpipe OpenGL 渲染..."
LIBGL_ALWAYS_SOFTWARE=1 timeout 10 glxgears >/tmp/glxgears.log 2>&1 &
echo $! > /tmp/glxgears.pid
sleep 1
cat /tmp/glxgears.log
echo "glxgears 已启动 (pid $(cat /tmp/glxgears.pid)), 画面应出现在 VNC 里"
EOF
  chmod +x "$GAME_DIR/run.sh"
fi

# ---------- 7. /init + /etc/rc.start ----------
echo "[7/8] /init (PID 1) + rc.start"
cat > "$ROOT/etc/rc.start" <<'EOF'
#!/bin/sh
# rc.start — 访客启动脚本
mount -t proc proc /proc
mount -t sysfs sys /sys
mount -t devtmpfs dev /dev
mount -t devpts devpts /dev/pts
mount -t tmpfs tmpfs /tmp
mount -t tmpfs tmpfs /run
exec 0</dev/console 1>/dev/console 2>&1

echo "=============================================="
echo " NexOS Guest Linux (Ubuntu 6.8 + Mesa llvmpipe)"
echo "=============================================="

# 网络 (QEMU SLIRP)
ifconfig lo up 2>/dev/null || true
ifconfig eth0 10.0.2.15 netmask 255.255.255.0 up 2>/dev/null || true
route add default gw 10.0.2.2 dev eth0 2>/dev/null || true
echo "IP: $(ifconfig eth0 2>/dev/null | grep 'inet ' || echo '?')"

# D-Bus
mkdir -p /run/dbus
dbus-daemon --system --fork 2>/dev/null || true

# Xvfb
Xvfb :0 -screen 0 1024x768x24 -ac -nolisten tcp +extension GLX +extension RENDER >/tmp/xvfb.log 2>&1 &
XVFB_PID=$!
sleep 2

# xkb
export DISPLAY=:0
if [ -f /usr/share/X11/xkb/compat/basic ]; then
  setxkbmap us 2>/dev/null || true
fi

# Mesa 配置 (强制软渲染)
export LIBGL_ALWAYS_SOFTWARE=1
export GALLIUM_DRIVER=llvmpipe
export EGL_PLATFORM=x11

# x11vnc
x11vnc -display :0 -rfbport 5900 -forever -nopw -shared \
       -bg -o /tmp/vnc.log -rfbdesktopname "NexOS-Guest" >/dev/null 2>&1 &
sleep 1
echo "VNC ready on :5900 (host port 15902)"

# 启动 glxgears (验证渲染链路)
glxinfo >/tmp/glxinfo.log 2>&1 &
sleep 1
echo "--- glxinfo (truncated) ---"
grep -E "OpenGL (vendor|renderer|version):" /tmp/glxinfo.log 2>/dev/null || true
echo "--- start glxgears ---"
glxgears >/tmp/glxgears.log 2>&1 &
GEARS_PID=$!
echo "glxgears pid=$GEARS_PID"

# 如果有 Unity 游戏, 启动
if [ -f /opt/game/run.sh ]; then
  echo "--- launching game ---"
  sh /opt/game/run.sh &
fi

echo "guest fully up — enter 'help' for commands"
echo "  glxinfo | head -20   — 查看 GPU/Mesa"
echo "  cat /tmp/vnc.log     — VNC 日志"
echo "  kill $GEARS_PID      — 停 gears"
echo "=============================================="

# 交互式 shell
exec /bin/sh
EOF
chmod +x "$ROOT/etc/rc.start"

# /init = busybox init applet (通过 symlink 名 "init" 触发)
cat > "$ROOT/etc/inittab" <<'EOF'
::sysinit:/bin/sh /etc/rc.start
::respawn:/bin/sh -c "exec /bin/sh </dev/console >/dev/console 2>&1"
::ctrlaltdel:/bin/umount -a -r
EOF

# ---------- 8. 打包 ----------
echo "[8/8] 打包 initramfs"
mkdir -p "$(dirname "$OUT")"
python3 "$SCRIPT_DIR/mk_initramfs.py" "$ROOT" "$OUT"

# 拷贝 Ubuntu 6.8 kernel
cp -a /boot/vmlinuz-6.8.0-146-generic "$VMlinuz_OUT"

echo ""
echo "=============================================="
echo " ✓ BUILD COMPLETE"
echo "   kernel : $(ls -lh "$VMlinuz_OUT" | awk '{print $5,$NF}')"
echo "   initrd : $(ls -lh "$OUT" | awk '{print $5,$NF}')"
echo "   rootfs : $(du -sh "$ROOT" | awk '{print $1}')"
echo ""
echo " 启动 QEMU:"
echo "  qemu-system-x86_64 -m 2048 -smp 2 -nographic \\"
echo "    -kernel $VMlinuz_OUT \\"
echo "    -initrd $OUT \\"
echo "    -append 'console=ttyS0 panic=-1 net.ifnames=0' \\"
echo "    -netdev user,id=n0,hostfwd=tcp::15902-:5900 \\"
echo "    -device virtio-net-pci,netdev=n0"
echo "=============================================="
