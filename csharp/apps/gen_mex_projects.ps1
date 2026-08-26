# Generate one standalone .mex project per managed application.
# Each project compiles the WHOLE managed core (NexOS.Forms + NexOS.Core +
# every Shell app) plus a tiny Program::Main that opens just that one app,
# producing a single self-contained assembly.  build_mex.ps1 then flattens
# it with tools/mex_pack.py into a NexOS .mex the kernel's MiniCLR runs.

$root = Split-Path -Parent $MyInvocation.MyCommand.Path   # csharp/apps
$shared = Join-Path $root "mex.shared.props"
$outRoot = Join-Path $root "mex"

# name -> NexOS.Forms.Kind member
$apps = @{
    "ControlPanel" = "ControlPanel"
    "FileExplorer" = "FileExplorer"
    "TaskManager"  = "TaskManager"
    "Terminal"     = "Terminal"
    "Calculator"   = "Calculator"
    "About"        = "About"
    "MemOptimizer" = "MemOptimizer"
    "Notepad"      = "Notepad"
    "Browser"      = "Browser"
    "AiSetup"      = "AiSetup"
    "AiAgent"      = "AiAgent"
    "Demo"         = "Demo"
}

foreach ($name in $apps.Keys) {
    $kind = $apps[$name]
    $dir = Join-Path $outRoot $name
    New-Item -ItemType Directory -Force -Path $dir | Out-Null

    $csproj = @"
<Project Sdk="Microsoft.NET.Sdk">
  <Import Project="$(Join-Path $root 'mex.shared.props')" />
  <PropertyGroup>
    <AssemblyName>$name</AssemblyName>
    <RootNamespace>App</RootNamespace>
  </PropertyGroup>
  <ItemGroup>
    <Compile Include="Program.cs" />
  </ItemGroup>
</Project>
"@
    Set-Content -Path (Join-Path $dir "$name.csproj") -Value $csproj -Encoding UTF8

    $program = @"
using NexOS.Forms;

namespace App
{
    // Standalone managed application entry point.
    // The kernel runs this via `clrapp $name.mex`:
    //   - Shell.Init() primes the managed static fields (they never
    //     auto-initialise under MiniCLR),
    //   - Host.OpenApp(kind) asks the kernel to open this one app window
    //     (native mforms_open -> resident NexOS.Forms.Shell::Open).
    // The kernel's native GUI loop then paints and forwards input to it.
    public static class Program
    {
        public static void Main()
        {
            Shell.Init();
            Host.OpenApp(Kind.$kind);
        }
    }
}
"@
    Set-Content -Path (Join-Path $dir "Program.cs") -Value $program -Encoding UTF8

    Write-Host "generated csharp/apps/mex/$name"
}
