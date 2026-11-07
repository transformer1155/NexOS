@echo off
setlocal EnableDelayedExpansion
REM =====================================================================
REM  nexos.bat  -  NexOS Developer Toolkit unified launcher
REM
REM    nexos              -> start system tray (gateway state on tray icon)
REM    nexos tray         -> same
REM    nexos gui          -> open WebView2 GUI
REM    nexos cli <args>   -> pass through to nexos_cli.py
REM    nexos stop         -> stop gateway
REM    nexos status       -> print gateway status and exit
REM =====================================================================

set "TOOLS=%~dp0tools"
set "TRAY=%TOOLS%\nexos_tray.py"

set "PY="
for %%P in ("E:\cache\py311\python.exe" "E:\cache\py312\python.exe" "D:\Program Files\Python313\python.exe" "D:\Program Files\Python312\python.exe") do (
  if exist %%P if not defined PY set "PY=%%~P"
)
if not defined PY (
  for /f "delims=" %%P in ('where python 2^>nul') do if not defined PY set "PY=%%P"
)
if not defined PY (
  echo [ERROR] python.exe not found. Install Python 3.10+ and add it to PATH.
  exit /b 1
)

if "%~1"=="" goto :tray
if /i "%~1"=="tray" goto :tray
if /i "%~1"=="gui" goto :gui
if /i "%~1"=="stop" goto :stop
if /i "%~1"=="status" goto :status
if /i "%~1"=="cli" goto :cli

:passthrough
goto :cli

:tray
echo [nexos] Python: %PY%
echo [nexos] starting system tray ... right-click the icon to start/stop gateway
"%PY%" "%TRAY%"
exit /b %ERRORLEVEL%

:gui
call :findfile nexos_gui.py
if not defined FOUND (
  echo [ERROR] nexos_gui.py not found under %TOOLS%
  exit /b 1
)
echo [nexos] GUI: %FOUND%
"%PY%" "%FOUND%"
exit /b %ERRORLEVEL%

:status
"%PY%" "%TRAY%" --selftest
exit /b %ERRORLEVEL%

:stop
"%PY%" -c "import sys,os;sys.path.insert(0,r'%TOOLS%');import importlib.util as u;s=u.spec_from_file_location('t',r'%TRAY%');m=u.module_from_spec(s);s.loader.exec_module(m);m.TrayApp().gw_stop()"
exit /b %ERRORLEVEL%

:cli
call :findfile nexos_cli.py
if not defined FOUND (
  echo [ERROR] nexos_cli.py not found under %TOOLS%
  exit /b 1
)
set "REST="
shift
:argloop
if "%~1"=="" goto :run
set "REST=!REST! "%~1""
shift
goto :argloop
:run
echo [nexos] CLI: %FOUND%
"%PY%" "%FOUND%" !REST!
exit /b %ERRORLEVEL%

:findfile
set "FOUND="
for /f "delims=" %%F in ('dir /b /s "%TOOLS%\%~1" 2^>nul') do if not defined FOUND set "FOUND=%%F"
exit /b 0
