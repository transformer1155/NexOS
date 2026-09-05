# Build every standalone per-application .mex and stage it into sfs_files/.
# Each .mex is a self-contained managed assembly (NexOS.Forms core + one app
# entry) flattened by tools/mex_pack.py, ready for the kernel's MiniCLR via
# `clrapp <Name>.mex`.

$ErrorActionPreference = "Stop"
$root  = Split-Path -Parent $MyInvocation.MyCommand.Path   # csharp/apps
$repo  = (Resolve-Path (Join-Path $root "..\..")).Path     # d:/MyOS/bootloader
$mexRoot = Join-Path $root "mex"
$sfs   = Join-Path $repo "sfs_files"
$tools = Join-Path $repo "tools"
$pack  = Join-Path $tools "mex_pack.py"

$apps = @("ControlPanel","FileExplorer","TaskManager","Terminal",
          "Calculator","About","MemOptimizer","Notepad","Browser",
          "AiSetup","AiAgent","Demo")

foreach ($name in $apps) {
    $dir = Join-Path $mexRoot $name
    $csproj = Join-Path $dir "$name.csproj"
    if (-not (Test-Path $csproj)) { Write-Warning "missing $csproj"; continue }

    Write-Host "==> build $name"
    dotnet build $csproj -c Release 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) { Write-Error "build failed: $name"; exit 1 }

    $dll = Join-Path $dir "bin/Release/net8.0/$name.dll"
    if (-not (Test-Path $dll)) { Write-Error "dll missing: $dll"; exit 1 }

    $mex = Join-Path $sfs "$name.mex"
    python $pack $dll $mex
    if ($LASTEXITCODE -ne 0) { Write-Error "mex_pack failed: $name"; exit 1 }
}

Write-Host "`nStaged 12 .mex files into: $sfs"
Get-ChildItem $sfs -Filter *.mex | ForEach-Object { "{0,-16} {1,8} bytes" -f $_.Name, $_.Length }
