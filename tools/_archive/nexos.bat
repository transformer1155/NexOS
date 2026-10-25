@echo off
setlocal
REM nexos.bat - Windows nexos_cli.py 入口（零 Python 环境也能跑）
REM 如果 dotnet 不在 PATH 里，这里自动补
set "DOTNET_DIR=C:\Program Files\dotnet"
if exist "%DOTNET_DIR%\dotnet.exe" (
  set "PATH=%DOTNET_DIR%;%PATH%"
)
python "%~dp0nexos_cli.py" %*
exit /b %ERRORLEVEL%
