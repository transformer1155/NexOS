Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public class W {
    public delegate bool EnumProc(IntPtr h, IntPtr lp);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr lp);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder sb, int max);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder sb, int max);
}
"@

$q = Start-Process -FilePath "D:\qemu\qemu-system-x86_64.exe" -ArgumentList @(
  "-m","512","-vga","cirrus","-display","gtk","-accel","tcg",
  "-drive","file=d:\MyOS\bootloader\build\os_v2.img,format=raw",
  "-serial","file:d:\MyOS\bootloader\build\ser_dbg.log"
) -PassThru

Start-Sleep -Seconds 30

"qemu pid=$($q.Id) exited=$($q.HasExited)"
"log exists: $(Test-Path d:\MyOS\bootloader\build\ser_dbg.log)"
if (Test-Path d:\MyOS\bootloader\build\ser_dbg.log) {
    "log tail: " + ((Get-Content d:\MyOS\bootloader\build\ser_dbg.log -Tail 4) -join ' | ')
}

$list = New-Object System.Collections.ArrayList
$null = [W]::EnumWindows({
    param($h, $lp)
    $p = 0
    [W]::GetWindowThreadProcessId($h, [ref]$p) | Out-Null
    if ($p -eq $q.Id) {
        $sb = New-Object System.Text.StringBuilder 256
        [W]::GetWindowText($h, $sb, 256) | Out-Null
        $cb = New-Object System.Text.StringBuilder 256
        [W]::GetClassName($h, $cb, 256) | Out-Null
        [void]$list.Add("hwnd=$h visible=$([W]::IsWindowVisible($h)) class='$($cb.ToString())' title='$($sb.ToString())'")
    }
    return $true
}, [IntPtr]::Zero)

"windows found: $($list.Count)"
$list | ForEach-Object { $_ }

Stop-Process -Id $q.Id -Force -ErrorAction SilentlyContinue
