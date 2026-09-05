@echo off
rem =====================================================================
rem  MiniOS Win11 Desktop - Windows 一键入口
rem  双击运行:   自动调用 WSL 执行 run.sh
rem  用法:  run.bat [build|run|test|install|clean|shots|help]
rem  例:    run.bat test    -> 无头截图
rem          run.bat run     -> 打开桌面 (需 WSLg / X 显示)
rem =====================================================================
setlocal

rem ---- 项目在 WSL 中的路径 ----
set WSL_DIR=/mnt/d/MyOS/win11-desktop

rem ---- 默认命令 ----
if "%~1"=="" (
    set CMD=build
) else (
    set CMD=%1
)

echo [*] WSL 执行: ./run.sh %CMD%
wsl.exe -e bash -c "cd %WSL_DIR% && chmod +x run.sh && ./run.sh %CMD%"

if errorlevel 1 (
    echo [!] 执行失败
    exit /b 1
)
endlocal
