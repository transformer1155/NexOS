#!/usr/bin/env bash
# build_guest_initrd.sh — 构建 Linux 访客 initrd（busybox + Xvfb + x11vnc）
#
# 产物:
#   /tmp/guestroot/                   访客根文件系统
#   linux_root/guest-initramfs.cpio.gz 最终 initrd（gzip newc cpio）
#
# 宿主依赖 (apt-get install):
#   busybox-static x11vnc xvfb xterm iproute2 cpio
#
# 访客启动方式 (QEMU 示例):
#   qemu-system-x86_64 -m 512 \
#     -kernel /boot/vmlinuz \
#     -initrd linux_root/guest-initramfs.cpio.gz \
#     -append "console=ttyS0 panic=-1" \
#     -netdev user,id=n0,hostfwd=tcp::15900-:5900 \
#     -device virtio-net-pci,netdev=n0 \
#     -nographic

set -euo pipefail

ROOT="${ROOT:-/tmp/guestroot}"
OUT="${OUT:-linux_root/guest-initramfs.cpio.gz}"

echo "==> 清理旧 rootfs: $ROOT"
rm -rf "$ROOT"

echo "==> 基础目录结构"
mkdir -p "$ROOT"/{bin,sbin,etc,proc,sys,dev,tmp,var,run,lib,x11,usr/bin,usr/sbin,usr/libexec}

echo "==> busybox (static) + applet symlinks"
cp /usr/bin/busybox "$ROOT/bin/busybox"
# /init 必须是 busybox **真实拷贝** 且 basename 精确为 "init"，
# 否则 PID 1 会走 ash 而不是 init applet（argv[0] basename 路由）
cp -a -L /usr/bin/busybox "$ROOT/init"
# 常用 applet
for app in sh mount sleep echo cat grep setsid mknod ifconfig udhcpc \
           mkdir ln ls ps kill rm awk sed head tail wc; do
  ln -sf busybox "$ROOT/bin/$app"
done

echo "==> 关键二进制"
for b in /usr/bin/Xvfb /usr/bin/x11vnc /usr/bin/xterm /usr/sbin/ip \
         /usr/bin/xkbcomp /usr/bin/xmodmap /usr/bin/xrdb /usr/bin/xset; do
  [ -f "$b" ] && cp "$b" "$ROOT/usr/bin/"
done
[ -f /usr/sbin/ip ] && cp /usr/sbin/ip "$ROOT/sbin/"

echo "==> 递归拷贝共享库 (ldd + dlopen 补漏)"
LIBD="$ROOT/lib/x86_64-linux-gnu"
mkdir -p "$LIBD"

copy_deps() {
  local bin="$1"
  ldd "$bin" 2>/dev/null | awk '{print $3}' | grep '^/' | sort -u | while read -r lib; do
    [ -z "$lib" ] && continue
    local real
    real=$(readlink -f "$lib")
    local target="$LIBD/$(basename "$real")"
    [ -f "$target" ] && continue
    cp -a "$real" "$target"
    copy_deps "$real"
  done
}

for b in /usr/bin/Xvfb /usr/bin/x11vnc /usr/bin/xterm /usr/sbin/ip \
         /usr/bin/xkbcomp /usr/bin/xmodmap /usr/bin/xrdb /usr/bin/xset; do
  [ -f "$b" ] && copy_deps "$b"
done

# dlopen 补漏：Xvfb 隐依赖 libbz2 / liblz4，x11vnc 需要 libvncclient
for pat in libbz2.so.1 libvncclient.so.1 liblz4.so.1 liblzo2.so.2 \
           libsnappy.so.1 libzstd.so.1 libvncserver.so.1 libXfont2.so.2 \
           libpixman-1.so.0 libxcb.so.1; do
  HOST=$(find /lib /usr/lib -name "$pat*" -type f 2>/dev/null | head -n 1)
  if [ -n "$HOST" ]; then
    cp -a "$HOST" "$LIBD/"
  fi
done

echo "==> 重建 soname symlinks"
cd "$LIBD" || exit 1
for real in lib*.so.*.*; do
  [ -f "$real" ] || continue
  # libGL.so.1.7.0 -> libGL.so.1
  major=$(echo "$real" | sed -E 's/(.*\.so)\.[0-9]+\.[0-9]+/\1/')
  ver=$(echo "$real"   | sed -E 's/(.*\.so\.([0-9]+))\.[0-9]+/\1/')
  [ "$major" = "$real" ] && continue
  ln -sf "$real" "$ver"
  [ "$ver" != "$real" ] && ln -sf "$ver" "$major"
done
cd - >/dev/null

echo "==> 动态链接器 (ELF .interp)"
LOADER_HOST=$(readlink -f /lib64/ld-linux-x86-64.so.2 2>/dev/null ||
              find /lib /usr/lib -name 'ld-linux-x86-64.so*' -type f 2>/dev/null | head -n 1)
if [ -n "$LOADER_HOST" ]; then
  LOADER_BASENAME=$(basename "$LOADER_HOST")
  mkdir -p "$ROOT/lib64"
  cp -a "$LOADER_HOST" "$ROOT/lib64/$LOADER_BASENAME"
fi

echo "==> /dev 节点"
mknod -m 666 "$ROOT/dev/null"    c 1 3   2>/dev/null
mknod -m 666 "$ROOT/dev/zero"    c 1 5   2>/dev/null
mknod -m 666 "$ROOT/dev/random"  c 1 8   2>/dev/null
mknod -m 666 "$ROOT/dev/urandom" c 1 9   2>/dev/null
mknod -m 600 "$ROOT/dev/console" c 5 1   2>/dev/null
mknod -m 600 "$ROOT/dev/ttyS0"   c 4 64  2>/dev/null
mkdir -p "$ROOT/dev/pts"

echo "==> /etc 配置"
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

echo "==> XKB 数据"
cp -a /usr/share/X11/xkb "$ROOT/usr/share/X11/" 2>/dev/null || true

echo "==> /etc/inittab + rc.start (busybox init)"
cat > "$ROOT/etc/inittab" <<'EOF'
::sysinit:/bin/sh /etc/rc.start
EOF
cat > "$ROOT/etc/rc.start" <<'EOF'
#!/bin/sh
# rc.start — 访客启动脚本
# 注意：devtmpfs 必须先挂载，才能 exec stdio 到 /dev/console
mount -t proc proc /proc
mount -t sysfs sys /sys
mount -t devtmpfs dev /dev
exec 0</dev/console 1>/dev/console 2>&1

echo "====== NexOS guest rc.start ======"

ifconfig lo up 2>&1
ifconfig eth0 10.0.2.15 up 2>&1
route add default gw 10.0.2.2 dev eth0 2>/dev/null || true
echo "net up"

Xvfb :0 -screen 0 1024x768x24 -nolisten tcp >/tmp/xvfb.log 2>&1 &
sleep 2
export DISPLAY=:0
x11vnc -display :0 -rfbport 5900 -forever -nopw -shared >/tmp/vnc.log 2>&1 &
echo "Xvfb+x11vnc up on :5900"

sleep infinity
EOF
chmod +x "$ROOT/etc/rc.start"

echo "==> 打包 initramfs"
python3 "$(dirname "$0")/mk_initramfs.py" "$ROOT" "$OUT"

echo "==> 校验 (CPIO newc magic)"
gzcat "$OUT" 2>/dev/null | head -c 6 | od -An -tx1
echo ""
ls -lh "$OUT"
echo "√ done — rootfs: $ROOT, initrd: $OUT"
