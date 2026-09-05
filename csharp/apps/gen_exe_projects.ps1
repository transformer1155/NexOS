# Generate one standalone .exe project per managed application.
# Each project imports the shared exe.shared.props so it compiles EXACTLY
# the same NexOS C# UI sources as the full WinHost shell, differing only in
# the entry point (AppHost.Run(<Kind>)).

$root = Split-Path -Parent $MyInvocation.MyCommand.Path   # csharp/apps
$shared = Join-Path $root "exe.shared.props"

# name -> Shell.Kind member
$apps = @{
    "ControlPanel" = "ControlPanel"  # Settings
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
    $dir = Join-Path $root $name
    New-Item -ItemType Directory -Force -Path $dir | Out-Null

    # per-app csproj
    $csproj = @"
<Project Sdk="Microsoft.NET.Sdk">
  <Import Project="$(Join-Path $root 'exe.shared.props')" />
  <PropertyGroup>
    <AssemblyName>$name</AssemblyName>
    <RootNamespace>WinHost.$name</RootNamespace>
  </PropertyGroup>
  <!-- Local Program.cs is picked up by the SDK default glob. -->
</Project>
"@
    Set-Content -Path (Join-Path $dir "$name.csproj") -Value $csproj -Encoding UTF8

    # per-app Program.cs
    $program = @"
using System;
using System.Windows.Forms;
using NexOS.Forms;

namespace WinHost.$name
{
    // Standalone .exe for the $name managed application.
    // Launches the app directly via AppHost - no desktop / taskbar / start menu.
    static class Program
    {
        [STAThread]
        static void Main()
        {
            AppHost.Run(Kind.$kind);
        }
    }
}
"@
    Set-Content -Path (Join-Path $dir "Program.cs") -Value $program -Encoding UTF8

    Write-Host "generated csharp/apps/$name"
}
