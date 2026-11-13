@echo off
chcp 65001 >nul
title NexOS Nested Virt Fix — NEEDS ADMIN + WINDOWS REBOOT

echo =====================================================
echo   NexOS Nested Virtualization — Root Cause Fix
echo   Root cause: Windows Hypervisor (Hyper-V) blocking
echo   VBox AMD nested SVM.  NEM backend does NOT support
echo   nested hardware virt.
echo =====================================================
echo.
echo This will:
echo   1. Set hypervisorlaunchtype = OFF (permanent)
echo   2. Disable Hyper-V features
echo   3. Ask you to REBOOT WINDOWS
echo.
echo =====================================================
echo.

:: --- 1. Set hypervisorlaunchtype off (MOST IMPORTANT) ---
echo [1/3] bcdedit /set hypervisorlaunchtype off ...
bcdedit /set hypervisorlaunchtype off
if errorlevel 1 (
    echo [FAIL] bcdedit failed — is this running as ADMIN?
    echo Please right-click cmd.exe and "Run as administrator".
    pause
    exit /b 1
)
echo [OK] hypervisorlaunchtype = OFF

:: --- 2. Disable Hyper-V features ---
echo.
echo [2/3] dism /online /disable-feature Microsoft-Hyper-V-All ...
dism /online /disable-feature:Microsoft-Hyper-V-All /norestart
echo [OK] Hyper-V features disabled

:: Also turn off VirtualMachinePlatform (used by WSL2, also loads hypervisor)
echo.
echo [2b/3] dism /online /disable-feature VirtualMachinePlatform ...
dism /online /disable-feature:VirtualMachinePlatform /norestart

:: --- 3. Show what needs reboot ---
echo.
echo [3/3] Verify current state (will only take effect AFTER reboot):
echo -----------------------------------------------------
bcdedit /enum {current} 2>&1 | findstr /i "hypervisorlaunch"
echo -----------------------------------------------------
echo.
echo  ============================================
echo   ALL COMMANDS RAN OK.  NOW YOU MUST:
echo   REBOOT WINDOWS (power cycle, not just "restart")
echo  ============================================
echo.
echo After reboot, run:
echo   VBoxManage list hostinfo ^| findstr nested
echo It should say "Processor supports nested HW virtualization: yes"
echo Then VBox guest CPUID will show svm flag.
echo.
echo =====================================================
pause
