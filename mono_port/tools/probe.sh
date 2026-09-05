#!/bin/bash
# Phase 0 可编译性普查脚本
# 用法: bash tools/probe.sh <子目录名>  例如 utils / metadata / mini-interp
set -u
cd "$(dirname "$0")/.."

WHAT="${1:-utils}"
case "$WHAT" in
  utils)      SRCDIR=../vendor/mono/utils ;;
  metadata)   SRCDIR=../vendor/mono/metadata ;;
  sgen)       SRCDIR=../vendor/mono/sgen ;;
  interp)     SRCDIR=../vendor/mono/mini/interp ;;
  *)          SRCDIR="$WHAT" ;;
esac

GI=$(gcc -m32 -print-file-name=include)
MONOROOT=../vendor
EGLIB=../vendor/mono/eglib
PALINC=pal/include

CFLAGS="-m32 -ffreestanding -nostdinc -isystem $GI -O0"
CFLAGS="$CFLAGS -I$PALINC -I$EGLIB -I$MONOROOT -I$MONOROOT/mono"
CFLAGS="$CFLAGS -DG_OS_UNIX -DUSE_GCC_ATOMIC_OPS -w"
# 两个上游自带的裁剪开关，不是我们发明的宏：
#   DISABLE_SOCKETS 连 icall-def.h 里的 System.Net.Sockets 一起摘掉，
#                   所以不会留下悬空 icall（MiniOS 没有网络栈）。
#   DISABLE_ATTACH  attach.c 自带 #else 分支提供 no-op 桩。
CFLAGS="$CFLAGS -DDISABLE_SOCKETS -DDISABLE_ATTACH"

mkdir -p build/probe build/probe_out
OUTD=build/probe_out
OKF=$OUTD/probe_${WHAT}_ok.txt
FAILF=$OUTD/probe_${WHAT}_fail.txt
ERRD=$OUTD/probe_${WHAT}_err
rm -rf "$ERRD"; mkdir -p "$ERRD"
: > "$OKF"; : > "$FAILF"

EMPTYF=$OUTD/probe_${WHAT}_empty.txt
: > "$EMPTYF"

ok=0; fail=0; empty=0
for f in $SRCDIR/*.c; do
  b=$(basename "$f" .c)
  o="build/probe/${WHAT}_${b}.o"
  if gcc $CFLAGS -c "$f" -o "$o" 2>"$ERRD/$b.txt"; then
    ok=$((ok+1)); echo "$b" >> "$OKF"
    # "编过了" != "编出了东西"。Mono 大量文件整份包在 #ifdef HAVE_XXX_GC /
    # HOST_WIN32 之类的开关里，开关没打开就编成 0 符号的空 .o，编译器一声
    # 不吭。曾经因此让整个 sgen 层（25/25 "PASS"）实际上什么都没产出。
    if [ "$(nm -g --defined-only "$o" 2>/dev/null | wc -l)" -eq 0 ]; then
      empty=$((empty+1)); echo "$b" >> "$EMPTYF"
    fi
  else
    fail=$((fail+1))
    msg=$(grep -m1 -E 'error|fatal' "$ERRD/$b.txt" | cut -c1-180)
    echo "$b :: $msg" >> "$FAILF"
  fi
done
echo "=== $WHAT : OK=$ok FAIL=$fail EMPTY=$empty ==="
if [ "$empty" -gt 0 ]; then
  echo "--- 空目标文件（编过但 0 个符号，检查 config.h 的开关）---"
  tr '\n' ' ' < "$EMPTYF"; echo
fi
echo "--- missing headers ---"
grep -h 'fatal error' "$ERRD"/*.txt 2>/dev/null \
  | grep -oE '[A-Za-z0-9_/.-]+\.h: No such' | sort | uniq -c | sort -rn | head -20
echo "--- top error kinds ---"
awk -F' :: ' '{print $2}' "$FAILF" | sed -E 's/[0-9]+/N/g; s/.[a-z_0-9]+.$//' \
  | sed -E "s/^.*(fatal error: .*)/\\1/" | sort | uniq -c | sort -rn | head -20
