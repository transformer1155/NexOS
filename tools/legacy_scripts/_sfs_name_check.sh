#!/bin/bash
cd /media/sf_NexOS/NexOS || exit 1
echo '=== build/sfs.img 身份 ==='
echo "  size=$(stat -c%s build/sfs.img) mtime=$(stat -c %y build/sfs.img | cut -c1-19) sha16=$(sha256sum build/sfs.img | cut -c1-16)"
echo
echo '=== 新短名模板是否已在镜像里（用 SFS 名字段 19 字符内的实际名字查）==='
for n in coop-bugfix coop-invalid bugfix-end-to-end _invalid-probe; do
  c=$(grep -c -a "$n" build/sfs.img 2>/dev/null || echo 0)
  echo "  '$n' -> $c"
done
echo
echo '=== 名字字段里出现的 SFS 条目名（含被截断的） ==='
strings build/sfs.img 2>/dev/null | grep -E '^coop-|^ai/cooperation' | sort -u | head -8
echo
echo '=== 目录项原始字节里搜 coop-（16/17 字符，应能完整存放）==='
grep -c -a 'coop-bugfix.json' build/sfs.img 2>/dev/null || true
grep -c -a 'coop-invalid.json' build/sfs.img 2>/dev/null || true
