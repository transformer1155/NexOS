@echo off
rem dotnet wrapper for pure-Windows NexOS builds (invoked via cmd.exe from MSYS2).
rem MSYS2 strips Windows env vars when spawning native processes, which breaks
rem NuGet's restore settings resolution; cmd.exe `set` re-establishes them and
rem is not subject to MSYS2 path conversion.
set PROGRAMDATA=C:\ProgramData
set USERPROFILE=C:\Users\trans
set APPDATA=C:\Users\trans\AppData\Roaming
set LOCALAPPDATA=C:\Users\trans\AppData\Local
set HOMEDRIVE=C:
set HOMEPATH=\Users\trans
set SystemRoot=C:\Windows
"C:\Program Files\dotnet\dotnet.exe" %*
