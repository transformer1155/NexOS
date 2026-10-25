#!/bin/bash
# =====================================================================
#  check_kernel_ai.sh  -  在 WSL / Linux 下编译检查 nexos_ai.cpp
# ---------------------------------------------------------------------
#  RULES.md 要求构建在 WSL 里做。这台机器上没有 Debian 发行版，但有
#  Ubuntu-24.04，工具链（g++ 13 / make / python3）完整可用。
#
#  只编译 nexos_ai.cpp 这一个 TU，用于快速验证内核侧改动的语法正确性，
#  不参与完整镜像构建（那要走 make BUILD=...）。
#
#  用法:
#      wsl -d Ubuntu-24.04 -e bash /mnt/e/NexOS/NexOS/tools/verify/check_kernel_ai.sh
# =====================================================================
set -o pipefail

# 仓库根目录（脚本在 tools/ 下）
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT" || exit 2

OUT="${TMPDIR:-/tmp}/nexos_ai_check.o"
rm -f "$OUT"

echo "== compiling nexos_ai.cpp (freestanding, -m32) =="
g++ -m32 -ffreestanding -fno-exceptions -fno-rtti \
    -fno-stack-protector -fno-pic -fno-pie -nostdlib \
    -O2 -Wall -Wextra -fpermissive -I. \
    -c nexos_ai.cpp -o "$OUT" 2>&1

RC=$?
echo "== exit: $RC =="
if [ $RC -eq 0 ]; then
    echo "OK: nexos_ai.cpp compiles"
    ls -l "$OUT"
else
    echo "FAIL: nexos_ai.cpp has compile errors"
fi
exit $RC
