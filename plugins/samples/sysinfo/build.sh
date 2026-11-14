#!/bin/sh
# =====================================================================
#  build.sh  -  plugins/samples/sysinfo 一键构建
# ---------------------------------------------------------------------
#  步骤（每一步失败都会立刻退出，绝不产出半成品包）：
#
#    1. mcp_scan.py  扫描 [McpTool] + 重新生成 Invoke 派发表 + --check
#    2. dotnet build MiniCLR 程序集（/nostdlib，netstandard2.0）
#    3. mex_pack.py  sysinfo.dll -> sysinfo.mex（扁平化，内核可加载）
#    4. nexbundle.py pack   目录 -> .nexbundle（全 STORE、确定性 ZIP）
#    5. nexbundle.py verify 重新打开产物做结构/清单/MEX 魔数校验
#
#  可选签名：
#      NEXBUNDLE_SIGN_KEY=<32 字节 hex 种子> ./build.sh
#
#  环境要求：
#      python3（仓库工具链）、dotnet SDK 8+
#      g++/cargo 不需要 —— 这个示例只验证托管插件链路。
# =====================================================================
set -e

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
OUT="$HERE/dist/sysinfo.suite-1.0.0.nexbundle"

PY=python3
command -v python3 >/dev/null 2>&1 || PY=python

DOTNET=dotnet
command -v dotnet >/dev/null 2>&1 || {
    if command -v dotnet.exe >/dev/null 2>&1; then DOTNET=dotnet.exe; else
        echo "error: 找不到 dotnet（需要 .NET SDK 8+）" >&2
        exit 2
    fi
}

cd "$HERE"

echo "==> [1/5] scan [McpTool]"
"$PY" "$ROOT/tools/misc/mcp_scan.py" \
    --src src \
    --out src/mcp_tools.json \
    --emit-dispatch src/SysInfoTools.Dispatch.g.cs \
    --class SysInfoTools --namespace SysInfo --sealed --check

echo "==> [2/5] dotnet build (MiniCLR, /nostdlib)"
"$DOTNET" build src/SysInfoPlugin.csproj -c Release --nologo -v quiet

echo "==> [3/5] mex_pack"
mkdir -p bundle/plugins/sysinfo
"$PY" "$ROOT/tools/mex_pack.py" \
    src/bin/Release/sysinfo.dll \
    bundle/plugins/sysinfo/sysinfo.mex \
    "SysInfo.PluginMain::Init"

echo "==> [4/5] nexbundle pack"
mkdir -p dist
if [ -n "$NEXBUNDLE_SIGN_KEY" ]; then
    "$PY" "$ROOT/tools/image/nexbundle.py" pack \
        --src bundle --out "$OUT" --expect-id sysinfo.suite \
        --sign "$NEXBUNDLE_SIGN_KEY"
else
    "$PY" "$ROOT/tools/image/nexbundle.py" pack \
        --src bundle --out "$OUT" --expect-id sysinfo.suite
fi

echo "==> [5/5] nexbundle verify"
if [ -n "$NEXBUNDLE_PUBKEY" ]; then
    "$PY" "$ROOT/tools/image/nexbundle.py" verify "$OUT" --pubkey "$NEXBUNDLE_PUBKEY"
else
    "$PY" "$ROOT/tools/image/nexbundle.py" verify "$OUT"
fi

echo
echo "done: $OUT"
ls -l "$OUT"
