#!/usr/bin/env bash
# =====================================================================
#  link_audit.sh - 跨库未定义符号核查
# ---------------------------------------------------------------------
#  "五个 .a 都 ar 成功了" 不等于 "能链接"。ar 只是打包，一个符号都不
#  解析。真正的证据是：把五库 defined 集合并起来，看 undefined 集合里
#  还剩谁。
#
#  用法:  bash tools/link_audit.sh [-v]
#         -v  连每个缺失符号的引用者一起列出来
#
#  输出分三类：
#    PAL 侧缺口   - 名字像 libc 的，说明 PAL 还没实现
#    Mono 内部缺口 - mono_*/sgen_*/mini_*，说明少编了源文件或少给了宏
#    其它         - 编译器/链接器内建，通常可忽略
# =====================================================================
set -u
cd "$(dirname "$0")/.." || exit 1

BUILD=build
LIBS="$BUILD/libmono_port.a $BUILD/libmono_utils.a $BUILD/libmono_meta.a \
      $BUILD/libmono_sgen.a $BUILD/libmono_interp.a"

for l in $LIBS; do
	[ -f "$l" ] || { echo "missing $l -- run 'make runtime' first"; exit 1; }
done

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# defined: 全局已定义符号（T/D/B/R/W/V/C 都算，弱符号也能满足引用）
nm -g --defined-only $LIBS 2>/dev/null \
	| awk '{ if (NF>=3) print $3 }' | sort -u > "$TMP/def.txt"

# undefined: 'U' 行
nm -g --undefined-only $LIBS 2>/dev/null \
	| awk '$1=="U" { print $2 }' | sort -u > "$TMP/und.txt"

comm -23 "$TMP/und.txt" "$TMP/def.txt" > "$TMP/missing.txt"

# 链接器自带，不是我们的活
grep -vx '_GLOBAL_OFFSET_TABLE_' "$TMP/missing.txt" > "$TMP/m2.txt"
mv "$TMP/m2.txt" "$TMP/missing.txt"

grep -E '^(mono_|sgen_|mini_|mint_|interp_|ves_)' "$TMP/missing.txt" > "$TMP/mono.txt"
grep -vE '^(mono_|sgen_|mini_|mint_|interp_|ves_)' "$TMP/missing.txt" > "$TMP/pal.txt"

echo "=== link audit ==============================================="
printf 'defined   : %s\n' "$(wc -l < "$TMP/def.txt")"
printf 'undefined : %s\n' "$(wc -l < "$TMP/und.txt")"
printf 'MISSING   : %s   (mono-internal %s / pal-side %s)\n' \
	"$(wc -l < "$TMP/missing.txt")" \
	"$(wc -l < "$TMP/mono.txt")" "$(wc -l < "$TMP/pal.txt")"
echo

echo "--- PAL side (libc-ish, we must implement) -------------------"
cat "$TMP/pal.txt"
echo
echo "--- mono internal (missing source or missing -D) -------------"
cat "$TMP/mono.txt"

if [ "${1:-}" = "-v" ]; then
	echo
	echo "--- referenced by --------------------------------------------"
	while read -r s; do
		[ -n "$s" ] || continue
		refs=$(nm -g --undefined-only -A $LIBS 2>/dev/null \
			| awk -v s="$s" '$0 ~ ("U +" s "$") { print $1 }' \
			| sed 's/.*://' | sort -u | tr '\n' ' ')
		printf '%-52s %s\n' "$s" "$refs"
	done < "$TMP/missing.txt"
fi

cp "$TMP/missing.txt" "$BUILD/link_missing.txt"
echo
echo "full list -> $BUILD/link_missing.txt"
