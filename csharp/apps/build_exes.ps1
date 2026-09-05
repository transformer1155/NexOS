# Build every standalone per-application .exe into build/exes/<Name>/.
# Each exe reuses the same NexOS C# UI sources as the full WinHost shell.

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path   # csharp/apps
$build = (Resolve-Path (Join-Path $root "..\..\build")).Path
$out  = Join-Path $build "exes"

$apps = @("ControlPanel","FileExplorer","TaskManager","Terminal",
          "Calculator","About","MemOptimizer","Notepad","Browser",
          "AiSetup","AiAgent","Demo")

foreach ($name in $apps) {
    $dir = Join-Path $root $name
    $csproj = Join-Path $dir "$name.csproj"
    if (-not (Test-Path $csproj)) { Write-Warning "missing $csproj"; continue }
    $dest = Join-Path $out $name
    Write-Host "==> building $name"
    dotnet publish $csproj -c Release -r win-x64 --self-contained false `
        -o $dest
    if ($LASTEXITCODE -ne 0) {
        Write-Error "build failed for $name"
        exit 1
    }
}

Write-Host "`nAll standalone exes built under: $out"
