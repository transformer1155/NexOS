#!/bin/bash
# check_k64_fit.sh —— 构建前自检：64 位内核是否放得进镜像、分区是否重叠
#
# 背景：kernel64.bin 位于 LBA 2048 与 SFS 之间的“缝隙”里，32 位 loader 只搬
# KERNEL64_SECTORS 个扇区。一旦内核涨过这个缝隙，dd 就会静默覆盖文件系统，
# 或把截断的镜像交给 switch_to_64bit() 导致 triple fault —— 属于“镜像长到
# 硬编码偏移里”的经典故障。Makefile 的 $(IMG) 规则已有守卫，本脚本在
# 动手改内核前就能提前发现，并额外检查分区重叠。
#
# 用法：bash tools/check_k64_fit.sh
cd "$(dirname "$0")/.." || exit 1

k=$(stat -c%s build/kernel64.bin 2>/dev/null)
[ -z "$k" ] && { echo "build/kernel64.bin 不存在，请先构建"; exit 1; }
secs=$(sed -n 's/^#define[[:space:]]*KERNEL64_SECTORS[[:space:]]*\([0-9]*\).*/\1/p' kernel.cpp | head -1)
sfs=$(grep -m1 '^SFS_LBA' Makefile | tr -dc '0-9')
lin=$(grep -m1 '^LINUX_SFS_LBA' Makefile | tr -dc '0-9')
alt=$(sed -n 's/^#define[[:space:]]*SFS_ALT_LBA[[:space:]]*\([0-9]*\).*/\1/p' kernel.cpp | head -1)
gap=$(( (sfs - 2048) * 512 ))
lim=$(( secs * 512 ))
[ "$lim" -gt "$gap" ] && lim=$gap

echo "kernel64.bin       = $k bytes"
echo "KERNEL64_SECTORS   = $secs ($(( secs * 512 )) bytes)"
echo "SFS_LBA            = $sfs  -> gap = $gap bytes"
echo "SFS_ALT_LBA(kernel)= $alt"
echo "LINUX_SFS_LBA      = $lin"
echo "effective limit    = $lim bytes"

rc=0
if [ "$k" -gt "$lim" ]; then
  echo "RESULT: *** OVERFLOW by $(( k - lim )) bytes — make 会在镜像组装时失败 ***"
  echo "        修法：调大 KERNEL64_SECTORS 并把 SFS_LBA / SFS_ALT_LBA 一并后移"
  rc=1
else
  echo "RESULT: OK, spare = $(( (lim - k) / 512 )) sectors"
fi

[ "$alt" != "$sfs" ] && { echo "WARNING: SFS_ALT_LBA($alt) 与 Makefile SFS_LBA($sfs) 不一致！"; rc=1; }

if [ -f build/sfs.img ] && [ -f build/linux_sfs.img ]; then
  ss=$(( $(stat -c%s build/sfs.img) / 512 ))
  ls_=$(( $(stat -c%s build/linux_sfs.img) / 512 ))
  echo "sfs.img            = $ss sectors (LBA $sfs..$(( sfs + ss )))"
  echo "linux_sfs.img      = $ls_ sectors (LBA $lin..$(( lin + ls_ )))"
  if [ "$lin" -lt "$(( sfs + ss ))" ]; then
    echo "WARNING: LINUX_SFS_LBA 落在主 SFS 区间内，dd 会覆盖主 SFS 的一部分！"
    rc=1
  fi
fi
exit $rc
