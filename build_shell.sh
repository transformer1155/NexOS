#!/usr/bin/env bash
cd /d/MyOS/bootloader
export PATH="$PATH:/c/Program Files/dotnet"
dotnet build csharp/apps/Shell/Shell.csproj -c Release --nologo > /tmp/cs.log 2>&1
echo "BUILD_EXIT=$?"
python3 tools/mex_pack.py csharp/apps/Shell/bin/Release/Shell.dll sfs_files/shell.mex "NexOS.Forms.Shell::Init" > /tmp/mex.log 2>&1
echo "MEX_DONE"
