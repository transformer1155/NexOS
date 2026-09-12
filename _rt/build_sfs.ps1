$ErrorActionPreference = "Continue"
$repo = "d:\MyOS\bootloader"
Set-Location $repo

# 1. rebuild Shell.dll
& "C:\Program Files\dotnet\dotnet.exe" build csharp/apps/Shell/Shell.csproj -c Release -v quiet --nologo 2>&1 | Out-Null
Write-Host "shell build rc=$LASTEXITCODE"

$dll = "csharp/apps/Shell/bin/Release/Shell.dll"
if (!(Test-Path $dll)) { Write-Host "ERROR: dll missing"; exit 1 }

# 2. repack shell.mex
python tools/mex_pack.py $dll sfs_files/shell.mex "NexOS.Forms.Shell::Init" 2>&1 | Out-Host
Write-Host "mex rc=$LASTEXITCODE"

# 3. regenerate textures + sfs.img
python tools/tex_pack.py 2>&1 | Out-Host
python tools/sfs_gen.py sfs_files build/sfs.img 2>&1 | Out-Host
Write-Host "sfs rc=$LASTEXITCODE  size=$((Get-Item build/sfs.img).Length)"
