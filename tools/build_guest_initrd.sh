#!/usr/bin/env bash
# build_guest_initrd.sh — 构建 Linux 访客 initrd（busybox + Xvfb + x11vnc）
#
# 产物:
#   /tmp/guestroot/                    访客根文件系统(可 inspect)
#   linux_root/guest-initramfs.cpio.gz 最终 initrd（gzip newc cpio）
#
# 宿主依赖 (apt-get install, 脚本会尽量自动补齐):
#   busybox-static(强烈建议静态) x11vnc xvfb xterm iproute2 cpio
#   (python3 用于打包；curl/wget 用于按需下载 busybox)
#
# 访客启动方式 (QEMU 示例):
#   qemu-system-x86_64 -m 512 \
#     -kernel /boot/vmlinuz \
#     -initrd linux_root/guest-initramfs.cpio.gz \
#     -append "console=ttyS0 panic=-1" \
#     -netdev user,id=n0,hostfwd=tcp::15900-:5900 \
#     -device virtio-net-pci,netdev=n0 \
#     -nographic
#
# 设计要点:
#   * /init 必须是 busybox 的【真实二进制拷贝】(不能用 symlink)，
#     内核把它当作 PID 1；busybox 以 init applet 启动并读 /etc/inittab。
#   * 真正的 proc/sys/dev 挂载与网卡/桌面拉起放在 /etc/rc.start 里，
#     由 busybox init 的 sysinit action 调用。
#   * rc.start 开头必须先把 devtmpfs 挂上，才能把自身 stdio exec 到
#     /dev/console（见下方 exec 0</dev/console 1>/dev/console 2>&1）。

set -euo pipefail

ROOT="${ROOT:-/tmp/guestroot}"
OUT="${OUT:-linux_root/guest-initramfs.cpio.gz}"
CACHE_DIR="$(cd "$(dirname "$0")" && pwd)/.busybox_cache"
mkdir -p "$CACHE_DIR"

# ---------------------------------------------------------------------------
# 1) 获取 busybox (优先静态 busybox-static；缺失则自动下载)
# ---------------------------------------------------------------------------
busybox_is_static() {
  # 静态链接的 busybox，ldd 会报 "not a dynamic executable"
  local out
  out=$(ldd "$1" 2>&1)
  case "$out" in
    *"not a dynamic executable"*) return 0 ;;
    *) return 1 ;;
  esac
}

ensure_busybox() {
  local cand found td deb f u

  # 1a) 系统已安装的 busybox —— 优先静态
  for cand in /usr/bin/busybox /bin/busybox /sbin/busybox; do
    [ -x "$cand" ] || continue
    if busybox_is_static "$cand"; then echo "$cand"; return 0; fi
    found="$cand"   # 记下动态版本作为兜底
  done
  if command -v busybox >/dev/null 2>&1; then
    cand=$(command -v busybox)
    if busybox_is_static "$cand"; then echo "$cand"; return 0; fi
    found="$cand"
  fi
  [ -n "${found:-}" ] && { echo "$found"; return 0; }

  # 1b) apt 提供的 busybox-static (.deb 解包)
  if command -v apt-get >/dev/null 2>&1; then
    td=$(mktemp -d)
    if ( cd "$td" && apt-get download busybox-static >/dev/null 2>&1 ); then
      deb=$(ls "$td"/*.deb 2>/dev/null | head -n1)
      if [ -n "$deb" ]; then
        ( cd "$td" && ar x "$deb" && tar -xf data.tar.* 2>/dev/null )
        f=$(find "$td" -type f -name busybox 2>/dev/null | head -n1)
        if [ -n "$f" ]; then
          cp -f "$f" "$CACHE_DIR/busybox"
          chmod +x "$CACHE_DIR/busybox"
          rm -rf "$td"
          echo "$CACHE_DIR/busybox"; return 0
        fi
      fi
    fi
    rm -rf "$td"
  fi

  # 1c) 复用上次下载缓存
  if [ -x "$CACHE_DIR/busybox" ] && "$CACHE_DIR/busybox" >/dev/null 2>&1; then
    echo "$CACHE_DIR/busybox"; return 0
  fi

  # 1d) 直接下载预编译静态 busybox (musl, 完全静态)
  local urls=(
    "${BUSYBOX_URL:-}"
    "https://busybox.net/downloads/binaries/1.35.0-x86_64-linux-musl/busybox"
    "https://busybox.net/downloads/binaries/1.36.1/busybox-x86_64"
  )
  for u in "${urls[@]}"; do
    [ -z "$u" ] && continue
    if command -v curl >/dev/null 2>&1 && curl -fsSL "$u" -o "$CACHE_DIR/busybox" 2>/dev/null; then
      :
    elif command -v wget >/dev/null 2>&1 && wget -qO "$CACHE_DIR/busybox" "$u" 2>/dev/null; then
      :
    else
      continue
    fi
    chmod +x "$CACHE_DIR/busybox"
    if "$CACHE_DIR/busybox" >/dev/null 2>&1; then
      echo "$CACHE_DIR/busybox"; return 0
    fi
  done

  echo "ERROR: 无法获取可用的 busybox (请手动安装 busybox-static 或设置 BUSYBOX_URL)" >&2
  return 1
}

echo "==> 获取 busybox"
BBX="$(ensure_busybox)" || exit 1
echo "    使用 busybox: $BBX ($(busybox_is_static "$BBX" && echo static || echo dynamic))"

# ---------------------------------------------------------------------------
# 2) 清理旧 rootfs + 基础目录树
# ---------------------------------------------------------------------------
echo "==> 清理旧 rootfs: $ROOT"
rm -rf "$ROOT"

echo "==> 基础目录结构"
mkdir -p "$ROOT"/{bin,sbin,etc,etc/init.d,proc,sys,dev,dev/pts,tmp,var,run,lib,lib64,usr/bin,usr/sbin,usr/lib,usr/libexec,usr/share/X11}
chmod 1777 "$ROOT/tmp"

# ---------------------------------------------------------------------------
# 3) 安装 busybox 与 /init (真实拷贝)
# ---------------------------------------------------------------------------
echo "==> busybox (static) + /init 真实拷贝 + applet symlinks"
cp -f "$BBX" "$ROOT/bin/busybox"
# /init 必须是 busybox 的【真实二进制拷贝】且 basename 精确为 "init"，
# 否则 PID 1 会按 argv[0] basename 走到 ash 而不是 init applet。
cp -a -L "$BBX" "$ROOT/init"
chmod +x "$ROOT/init"

for app in sh mount umount sleep echo cat grep setsid mknod ifconfig route \
           udhcpc ip ln ls ps kill rm awk sed head tail wc mkdir rmdir \
           chmod chown test true false touch dd sync reboot poweroff \
           hostname dmesg free top; do
  ln -sf busybox "$ROOT/bin/$app"
done

# ---------------------------------------------------------------------------
# 4) 关键二进制 (Xvfb / x11vnc / xterm / ip 等)
# ---------------------------------------------------------------------------
echo "==> 关键二进制"
BIN_LIST="/usr/bin/Xvfb /usr/bin/x11vnc /usr/bin/xterm /usr/bin/xkbcomp \
          /usr/bin/xmodmap /usr/bin/xrdb /usr/bin/xset /usr/sbin/ip"
for b in $BIN_LIST; do
  if [ -f "$b" ]; then
    cp -f "$b" "$ROOT/usr/bin/"
  else
    echo "    [warn] 缺失二进制: $b (apt-get install xvfb x11vnc xterm iproute2)"
  fi
done
[ -f /usr/sbin/ip ] && cp -f /usr/sbin/ip "$ROOT/sbin/"

# ---------------------------------------------------------------------------
# 5) 递归收集依赖库 (ldd) + dlopen 隐依赖补漏 + soname 软链
# ---------------------------------------------------------------------------
echo "==> 递归拷贝共享库 (ldd + dlopen 补漏)"
LIBD="$ROOT/lib/x86_64-linux-gnu"
mkdir -p "$LIBD"

copy_deps() {
  local bin="$1" lib real target
  ldd "$bin" 2>/dev/null | awk '{print $3}' | grep '^/' | sort -u | while read -r lib; do
    [ -z "$lib" ] && continue
    real=$(readlink -f "$lib")
    [ -f "$real" ] || continue
    target="$LIBD/$(basename "$real")"
    [ -f "$target" ] && continue
    cp -a "$real" "$target"
    copy_deps "$real"
  done
}

# 包含 busybox 自身(若它是动态的，需要 libc 等)
copy_deps "$BBX" 2>/dev/null || true
for b in /usr/bin/Xvfb /usr/bin/x11vnc /usr/bin/xterm /usr/sbin/ip \
         /usr/bin/xkbcomp /usr/bin/xmodmap /usr/bin/xrdb /usr/bin/xset; do
  [ -f "$b" ] && copy_deps "$b"
done

# dlopen 隐依赖：Xvfb 经常 dlopen libbz2 / liblz4 / libXfont2 等；
# x11vnc 需要 libvncclient / libvncserver。ldd 抓不到，逐一补齐。
echo "==> dlopen 隐依赖补漏"
for pat in libbz2.so.1 liblz4.so.1 liblzo2.so.2 libsnappy.so.1 libzstd.so.1 \
           libvncclient.so.1 libvncserver.so.1 libXfont2.so.2 libpixman-1.so.0 \
           libxcb.so.1 libfontenc.so.1 libfreetype.so.6 libICE.so.6 libSM.so.6 \
           libX11.so.6 libXext.so.6 libXfixes.so.3 libXdamage.so.1 libXrender.so.1 \
           libXt.so.6 libXmu.so.6 libXdmcp.so.6 libXau.so.6 libGL.so.1 libdrm.so.2 \
           libpcre.so.3 libselinux.so.1 liblzma.so.5 libtirpc.so.3 libnsl.so.2 \
           libgcc_s.so.1 libdl.so.2 libm.so.6 libpthread.so.0 libc.so.6 \
           libresolv.so.2 librt.so.1; do
  HOST=$(find /lib /usr/lib -name "$pat*" -type f 2>/dev/null | head -n 1)
  if [ -n "$HOST" ]; then
    cp -a "$HOST" "$LIBD/" 2>/dev/null || true
  fi
done

echo "==> 重建 soname symlink 链 (libfoo.so.1.2.3 -> libfoo.so.1 -> libfoo.so)"
cd "$LIBD" || exit 1
for real in lib*.so.*.*; do
  [ -f "$real" ] || continue
  # real = libGL.so.1.7.0
  ver=$(echo "$real" | sed -E 's/\.[0-9]+$//')          # -> libGL.so.1
  major=$(echo "$real" | sed -E 's/\.[0-9]+\.[0-9]+$//') # -> libGL.so
  [ "$ver" = "$real" ] && continue
  ln -sf "$real" "$ver"
  [ "$major" != "$real" ] && [ "$major" != "$ver" ] && ln -sf "$ver" "$major"
done
cd - >/dev/null

echo "==> 生成 ld.so.cache (确保运行时能解析 /lib/x86_64-linux-gnu 下的库)"
if command -v ldconfig >/dev/null 2>&1; then
  ldconfig -r "$ROOT" 2>/dev/null || true
fi

# ---------------------------------------------------------------------------
# 6) 动态链接器 (ELF .interp)
# ---------------------------------------------------------------------------
echo "==> 动态链接器 (ld-linux)"
LOADER_HOST=$(readlink -f /lib64/ld-linux-x86-64.so.2 2>/dev/null ||
              find /lib /usr/lib -name 'ld-linux-x86-64.so*' -type f 2>/dev/null | head -n 1)
if [ -n "$LOADER_HOST" ]; then
  LOADER_BASENAME=$(basename "$LOADER_HOST")
  mkdir -p "$ROOT/lib64" "$ROOT/lib"
  cp -a "$LOADER_HOST" "$ROOT/lib64/$LOADER_BASENAME"
  # 部分二进制 .interp 指向 /lib/ld-linux-x86-64.so.2，补一条软链
  ln -sf "../lib64/$LOADER_BASENAME" "$ROOT/lib/ld-linux-x86-64.so.2" 2>/dev/null || true
fi

# ---------------------------------------------------------------------------
# 7) /dev 静态节点 (devtmpfs 挂上后会被覆盖，这里作为兜底)
# ---------------------------------------------------------------------------
echo "==> /dev 节点"
mknod -m 666 "$ROOT/dev/null"    c 1 3   2>/dev/null || true
mknod -m 666 "$ROOT/dev/zero"    c 1 5   2>/dev/null || true
mknod -m 666 "$ROOT/dev/random"  c 1 8   2>/dev/null || true
mknod -m 666 "$ROOT/dev/urandom" c 1 9   2>/dev/null || true
mknod -m 600 "$ROOT/dev/console" c 5 1   2>/dev/null || true
mknod -m 600 "$ROOT/dev/ttyS0"   c 4 64  2>/dev/null || true
mkdir -p "$ROOT/dev/pts"

# ---------------------------------------------------------------------------
# 8) /etc 基础配置
# ---------------------------------------------------------------------------
echo "==> /etc 配置"
cat > "$ROOT/etc/ld.so.conf" <<'EOF'
/lib
/usr/lib
/lib64
/lib/x86_64-linux-gnu
/usr/lib/x86_64-linux-gnu
EOF
cat > "$ROOT/etc/passwd" <<'EOF'
root:x:0:0:root:/root:/bin/sh
EOF
cat > "$ROOT/etc/group" <<'EOF'
root:x:0:
EOF
cat > "$ROOT/etc/hosts" <<'EOF'
127.0.0.1   localhost
EOF
cat > "$ROOT/etc/fstab" <<'EOF'
proc    /proc   proc    defaults        0 0
sysfs   /sys    sysfs   defaults        0 0
EOF

echo "==> XKB 数据"
mkdir -p "$ROOT/usr/share/X11"
cp -a /usr/share/X11/xkb "$ROOT/usr/share/X11/" 2>/dev/null || true

# ---------------------------------------------------------------------------
# 9) /etc/inittab + /etc/rc.start (busybox init)
# ---------------------------------------------------------------------------
echo "==> /etc/inittab + rc.start"
cat > "$ROOT/etc/inittab" <<'EOF'
::sysinit:/bin/sh /etc/rc.start
EOF

cat > "$ROOT/etc/rc.start" <<'EOF'
#!/bin/sh
# rc.start — 访客启动脚本，由 busybox init (PID 1) 的 sysinit action 调用。
#
# 关键点：PID 1 的 stdio 重定向
#   内核把 /dev/console 作为 PID 1 的 0/1/2，但 rc.start 是 PID 1 派生的
#   子 shell，其 fds 需要显式重定向到控制台，否则 Xvfb / x11vnc 的日志与
#   错误信息会丢失。devtmpfs 必须先挂载，/dev/console 才存在，故顺序为：
#     mount devtmpfs -> exec 0</dev/console 1>/dev/console 2>&1
#
# 挂载 proc / sys / dev（busybox init 本身不挂载这些）
mount -t proc     proc     /proc
mount -t sysfs    sys      /sys
mount -t devtmpfs dev      /dev
mount -t devpts   devpts   /dev/pts 2>/dev/null || true

# === PID 1(stdio) 重定向：把 rc.start 及其所有子进程的 0/1/2 钉到控制台 ===
exec 0</dev/console 1>/dev/console 2>&1

echo "====== NexOS guest rc.start ======"

# 启动网卡：优先 DHCP(udhcpc)，失败则回落 QEMU user-net 静态地址
if ! udhcpc -i eth0 -n -q -t 3 >/tmp/udhcpc.log 2>&1; then
  ifconfig lo up
  ifconfig eth0 10.0.2.15 netmask 255.255.255.0 up
  route add default gw 10.0.2.2 dev eth0 2>/dev/null || true
fi
echo "net up"

# 启动虚拟显示 Xvfb :0 (1024x768 24bit)
Xvfb :0 -screen 0 1024x768x24 -nolisten tcp >/tmp/xvfb.log 2>&1 &
sleep 2

# 启动 VNC Server，把 :0 暴露到 5900，永久/共享/无密码
export DISPLAY=:0
x11vnc -display :0 -rfbport 5900 -forever -shared -nopw >/tmp/vnc.log 2>&1 &
echo "Xvfb + x11vnc up on :5900"

# 保持 init 存活（busybox init 的 sysinit 不返回，整个访客持续运行）
sleep infinity
EOF
chmod +x "$ROOT/etc/rc.start"

# ---------------------------------------------------------------------------
# 10) 校验关键产物存在
# ---------------------------------------------------------------------------
[ -x "$ROOT/init" ]      || { echo "ERROR: /init 缺失"; exit 1; }
[ -f "$ROOT/bin/busybox" ] || { echo "ERROR: busybox 缺失"; exit 1; }
command -v python3 >/dev/null 2>&1 || { echo "ERROR: 需要 python3"; exit 1; }

# ---------------------------------------------------------------------------
# 11) 打包 initramfs
# ---------------------------------------------------------------------------
mkdir -p "$(dirname "$OUT")"
echo "==> 打包 initramfs -> $OUT"
python3 "$(dirname "$0")/mk_initramfs.py" "$ROOT" "$OUT"

echo "==> 校验 (CPIO newc magic 应为 30 37 30 37 30 31 = '070701')"
gzcat "$OUT" 2>/dev/null | head -c 6 | od -An -tx1
echo ""
ls -lh "$OUT"
echo "√ done — rootfs: $ROOT, initrd: $OUT"
