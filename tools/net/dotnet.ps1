# dotnet wrapper for pure-Windows NexOS builds.
#
# MSYS2 strips Windows environment variables when it spawns native processes,
# which breaks NuGet's restore-settings resolution ("Value cannot be null.
# (Parameter 'path1')" at NuGet.targets:782).  PowerShell re-establishes the
# variables in its own process environment before launching dotnet.exe, and
# PowerShell's env manipulation is not subject to MSYS2 path conversion.
$env:PROGRAMDATA   = 'C:\ProgramData'
$env:USERPROFILE   = 'C:\Users\trans'
$env:APPDATA       = 'C:\Users\trans\AppData\Roaming'
$env:LOCALAPPDATA  = 'C:\Users\trans\AppData\Local'
$env:HOMEDRIVE     = 'C:'
$env:HOMEPATH      = '\Users\trans'
$env:SystemRoot    = 'C:\Windows'
& 'C:\Program Files\dotnet\dotnet.exe' @args
exit $LASTEXITCODE
