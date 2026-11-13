# Nexus nested virt root-cause diagnostic (RUN AS ADMIN)
$ErrorActionPreference = 'Continue'

Write-Host "=========================================="
Write-Host " NexOS Nested Virtualization Root-Cause Diag"
Write-Host "=========================================="

Write-Host "`n=== 1. bcdedit hypervisorlaunchtype ==="
bcdedit /enum {current} 2>&1 | Select-String -Pattern 'hypervisorlaunch|launchtype|bootstatus'

Write-Host "`n=== 2. IsHypervisorPresent (kernel32) ==="
Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public class HVCheck {
    [DllImport("kernel32.dll", CallingConvention=CallingConvention.StdCall)]
    public static extern bool IsHypervisorPresent();
}
"@
Write-Host ("IsHypervisorPresent: " + [HVCheck]::IsHypervisorPresent())

Write-Host "`n=== 3. Hyper-V optional features ==="
foreach ($f in @(
    'Microsoft-Hyper-V','Microsoft-Hyper-V-All','Microsoft-Hyper-V-Hypervisor',
    'Microsoft-Hyper-V-Services','Microsoft-Hyper-V-VMM','VirtualMachinePlatform'
)) {
    try {
        $s = (Get-WindowsOptionalFeature -Online -FeatureName $f).State
    } catch { $s = 'ERROR' }
    Write-Host ("  {0,-40} {1}" -f $f, $s)
}

Write-Host "`n=== 4. DeviceGuard / VBS / HVCI ==="
try {
    $c = Get-CimInstance -Namespace Root\Microsoft\Windows\DeviceGuard -ClassName Win32_DeviceGuard
    Write-Host ("  SecurityServicesRunning (bitmask): 0x{0:X8}" -f $c.SecurityServicesRunning)
    Write-Host ("    bit0 VBS:       " + (if($c.SecurityServicesRunning -band 1){'ON'}else{'off'}))
    Write-Host ("    bit1 CredGuard: " + (if($c.SecurityServicesRunning -band 2){'ON'}else{'off'}))
    Write-Host ("    bit2 HVCI:      " + (if($c.SecurityServicesRunning -band 4){'ON'}else{'off'}))
    Write-Host ("  VBS status: " + $c.VirtualizationBasedSecurityStatus)
    Write-Host ("  MemoryIntegrityEnabled (UEFI): " + $c.MemoryIntegrityEnabled)
    Write-Host ("  MemoryIntegrityPolicyEnforced: " + $c.MemoryIntegrityPolicyEnforced)
} catch { Write-Host "  DeviceGuard query failed: $_" }

Write-Host "`n=== 5. VBox hostinfo virtualization ==="
& "C:\Program Files\Oracle\VBoxManage.exe" list hostinfo 2>&1 | Select-String -Pattern 'Hypervisor|VMM|Virtualization|HWVirt|nested'

Write-Host "`n=== 6. CPU 32-bit vs 64-bit flags (WMI) ==="
$cpu = Get-CimInstance Win32_Processor
Write-Host ("  Name: " + $cpu.Name)
Write-Host ("  VirtualizationFirmwareEnabled: " + $cpu.VirtualizationFirmwareEnabled)
Write-Host ("  VirtualizationSupports: " + $cpu.VirtualizationSupports)
Write-Host ("  VirtualizationPS: " + $cpu.VirtualizationPS)
Write-Host ("  Extensions: " + ($cpu.Extensions -join ','))

Write-Host "`n=== 7. VBox VM CPU section (Linux VM) ==="
$vbox_xml = "$env:USERPROFILE\VirtualBox VMs\Linux\Linux.vbox"
if (Test-Path $vbox_xml) {
    [xml]$doc = Get-Content $vbox_xml
    $cpu = $doc.VirtualBox.GetElementsByTagName("CPU")
    Write-Host ("  NestedHWVirt enabled: " + $cpu.NestedHWVirt.enabled)
    Write-Host ("  HardwareVirtExLargePages enabled: " + $cpu.HardwareVirtExLargePages.enabled)
    Write-Host ("  PAE enabled: " + $cpu.PAE.enabled)
    Write-Host ("  LongMode enabled: " + $cpu.LongMode.enabled)
    $cpuid_tree = $cpu.CpuIdTree
    if ($cpuid_tree) {
        foreach ($leaf in $cpuid_tree.CpuIdLeaf) {
            Write-Host ("  CPUID override: id={0} eax={1:X8} ebx={2:X8} ecx={3:X8} edx={4:X8}" -f $leaf.id,$leaf.eax,$leaf.ebx,$leaf.ecx,$leaf.edx)
        }
    } else { Write-Host "  (no CPUID override nodes)" }
} else { Write-Host "  VBox XML not found at $vbox_xml" }

Write-Host "`n=== DONE ==="
