@echo off
chcp 65001 >nul
echo ============================================
echo  Nested Virt Root-Cause: Hypervisor Finder
echo  RUN AS ADMIN
echo ============================================
echo.

echo === 1. bcdedit hypervisorlaunchtype ===
bcdedit /enum {current} 2>&1 | findstr /i "hypervisorlaunch launchtype bootstatus"
echo.

echo === 2. Optional Features (Hyper-V) ===
dism /online /get-featureinfo /featurename:Microsoft-Hyper-V-All 2>&1 | findstr /i "State hypervisor"
dism /online /get-featureinfo /featurename:VirtualMachinePlatform 2>&1 | findstr /i "State"
dism /online /get-featureinfo /featurename:Microsoft-Windows-Subsystem-Linux 2>&1 | findstr /i "State"
echo.

echo === 3. DeviceGuard / MemoryIntegrity / HVCI ===
powershell -NoProfile -Command "Get-CimInstance -Namespace Root\Microsoft\Windows\DeviceGuard -ClassName Win32_DeviceGuard | Format-List SecurityServicesRunning,VirtualizationBasedSecurityStatus,MemoryIntegrityEnabled,MemoryIntegrityPolicyEnforced"
echo.

echo === 4. Hypervisor running check (service list) ===
sc query vmcompute 2>&1 | findstr STATE
sc query hvhost 2>&1 | findstr STATE
echo.

echo === 5. VBox log WHv status ===
findstr /i "WHv.*HypervisorPresent" "%USERPROFILE%\VirtualBox VMs\Linux\Logs\VBox.log.1" 2>&1
echo.

echo === 6. WHv IsHypervisorPresent (kernel32) ===
powershell -NoProfile -Command "Add-Type @"
using System; using System.Runtime.InteropServices;
public class HV { [DllImport("kernel32.dll")] public static extern bool IsHypervisorPresent(); }
"@; [HV]::IsHypervisorPresent()"
echo.

echo === DONE ===
echo.
echo IF ANYTHING IS ON / PRESENT / TRUE:
echo   bcdedit /set hypervisorlaunchtype off
echo   dism /online /disable-feature Microsoft-Hyper-V-All /norestart
echo   (turn off Memory Integrity in Windows Security)
echo   THEN WINDOWS REBOOT
echo ============================================
pause
