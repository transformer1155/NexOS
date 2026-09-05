#!/usr/bin/env bash
# =====================================================================
#  check_config.sh - 揪出 config.h 里自相矛盾的宏
# ---------------------------------------------------------------------
#  为什么需要它：C 预处理器按行序生效，后面的 #undef 会悄悄干掉前面的
#  #define。而 Mono 用的是 #ifdef HAVE_X 而不是 #if HAVE_X，所以这种
#  自伤【不会有任何警告】——只会表现成"我明明补了 sys/select.h，
#  console-unix.c 怎么还说 fd_set 未知"。
#
#  我们真踩过：HAVE_SYS_WAIT_H / HAVE_LOCALE_H / HAVE_POLL_H 三个
#  在第 55~61 行 #define，又在第 69~80 行被 #undef，白白多绕了几轮。
#
#  用法:  bash tools/check_config.sh      （make runtime 会自动跑）
# =====================================================================
set -u
cd "$(dirname "$0")/.." || exit 1

H=pal/include/config.h
[ -f "$H" ] || { echo "no $H"; exit 1; }

dups=$(grep -oE '^#(define|undef)[ \t]+[A-Za-z0-9_]+' "$H" \
       | awk '{print $2}' | sort | uniq -d)

if [ -n "$dups" ]; then
	echo "config.h: 同一个宏出现多次，后面的会覆盖前面的："
	for d in $dups; do
		echo "  $d"
		grep -nE "^#(define|undef)[ \t]+$d\b" "$H" | sed 's/^/      /'
	done
	exit 1
fi

echo "config.h: OK (no duplicate macros)"
