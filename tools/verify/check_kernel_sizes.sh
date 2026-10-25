#!/bin/bash
# =====================================================================
#  check_kernel_sizes.sh  -  内核体积 / 布局硬约束自检
# ---------------------------------------------------------------------
#  等价于 tools/verify/check_k64_fit.sh 的核心检查，但不依赖 CRLF 敏感的行尾，
#  且能接受自定义产物目录（原脚本硬编码 build/）。
#
#  RULES.md 规定的约束：
#    * 32 位 kernel.bin <= 588800 字节（EBDA 上限 0x9FC00）
#    * 2048 + KERNEL64_SECTORS <= SFS_LBA
#
#  用法:
#    bash tools/verify/check_kernel_sizes.sh [构建目录]
#    默认构建目录：build
# =====================================================================
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT" || exit 2

BUILD="${1:-build}"
K32="$BUILD/kernel.bin"
K64="$BUILD/kernel64.bin"

fail=0

if [ ! -f "$K32" ]; then
    echo "SKIP  32-bit kernel.bin not found at $K32"
else
    s=$(stat -c%s "$K32")
    if [ "$s" -le 588800 ]; then
        echo "PASS  32-bit kernel.bin = $s bytes (<= 588800, EBDA limit)"
    else
        echo "FAIL  32-bit kernel.bin = $s bytes (> 588800)"
        fail=1
    fi
fi

if [ ! -f "$K64" ]; then
    echo "SKIP  64-bit kernel64.bin not found at $K64"
else
    s=$(stat -c%s "$K64")
    sectors=$(( (s + 511) / 512 ))
    echo "INFO  64-bit kernel64.bin = $s bytes ($sectors sectors)"

    # 从 kernel.cpp 读 KERNEL64_SECTORS 与 SFS_LBA 相关常量。
    # 必须先剔除注释行：kernel.cpp 里既有 "KERNEL64_SECTORS=1460" 的
    # 历史注释，也有 "#define KERNEL64_SECTORS 4096" 的真实定义。
    k64s=$(grep -v '^[[:space:]]*//' kernel.cpp \
           | grep -oP '#define\s+KERNEL64_SECTORS\s+\K[0-9]+' | head -1)
    sfs=$(grep -v '^[[:space:]]*#' Makefile \
          | grep -oP '^SFS_LBA\s*:?=\s*\K[0-9]+' | head -1)
    [ -z "$sfs" ] && sfs=$(grep -oP 'SFS_LBA[[:space:]]*[:?]?=[[:space:]]*\K[0-9]+' Makefile | head -1)

    if [ -n "$k64s" ]; then
        echo "INFO  KERNEL64_SECTORS = $k64s"
        if [ "$sectors" -le "$k64s" ]; then
            echo "PASS  64-bit image fits in KERNEL64_SECTORS"
        else
            echo "FAIL  64-bit image ($sectors sectors) exceeds KERNEL64_SECTORS ($k64s)"
            fail=1
        fi
        if [ -n "$sfs" ]; then
            end=$(( 2048 + k64s ))
            echo "INFO  2048 + KERNEL64_SECTORS = $end, SFS_LBA = $sfs"
            if [ "$end" -le "$sfs" ]; then
                echo "PASS  64-bit kernel does not overlap the SFS at LBA $sfs"
            else
                echo "FAIL  64-bit kernel overlaps SFS (end $end > SFS_LBA $sfs)"
                fail=1
            fi
        fi
    else
        echo "SKIP  KERNEL64_SECTORS not found in kernel.cpp"
    fi
fi

echo
if [ "$fail" -eq 0 ]; then
    echo "== capacity check OK =="
else
    echo "== capacity check FAILED =="
fi
exit "$fail"
