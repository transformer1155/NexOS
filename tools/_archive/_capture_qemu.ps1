Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public class WinCap {
    public delegate bool EnumProc(IntPtr h, IntPtr lp);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr lp);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder sb, int max);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder sb, int max);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L; public int T; public int R; public int B; }
}
"@

$img = "d:\MyOS\bootloader\build\os_v2.img"
$log = "d:\MyOS\bootloader\build\ser_shot.log"
if (Test-Path $log) { Remove-Item $log -Force }

$q = Start-Process -FilePath "D:\qemu\qemu-system-x86_64.exe" -ArgumentList @(
  "-m","512","-vga","cirrus","-display","gtk","-accel","tcg",
  "-drive","file=$img,format=raw","-serial","file:$log"
) -PassThru

$deadline = (Get-Date).AddSeconds(150)
$ready = $false
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 1500
    if (Test-Path $log) {
        $c = Get-Content $log -Raw -ErrorAction SilentlyContinue
        if ($c -and $c.Contains("Cirrus HW cursor enabled")) { $ready = $true; break }
    }
    if ($q.HasExited) { break }
}
if ($ready) { Start-Sleep -Seconds 8 }

# Enumerate windows of the QEMU process (inline scriptblock - the closure form works).
$found = New-Object System.Collections.ArrayList
$null = [WinCap]::EnumWindows({
    param($h, $lp)
    $p = 0
    [WinCap]::GetWindowThreadProcessId($h, [ref]$p) | Out-Null
    if ($p -eq $q.Id) {
        $st = New-Object System.Text.StringBuilder 256
        [WinCap]::GetWindowText($h, $st, 256) | Out-Null
        $sc = New-Object System.Text.StringBuilder 256
        [WinCap]::GetClassName($h, $sc, 256) | Out-Null
        $t = $st.ToString()
        $vis = [WinCap]::IsWindowVisible($h)
        if ($vis -and $t -match "QEMU" -and $sc.ToString() -eq "gdkWindowToplevel") {
            [void]$found.Add($h)
        }
    }
    return $true
}, [IntPtr]::Zero)

# Pick the largest matching window (the guest display), ignoring tiny helper windows.
$best = [IntPtr]::Zero
$bestArea = 0
foreach ($h in $found) {
    $rr = New-Object WinCap+RECT
    [WinCap]::GetWindowRect($h, [ref]$rr) | Out-Null
    $a = ($rr.R - $rr.L) * ($rr.B - $rr.T)
    if ($a -gt $bestArea) { $bestArea = $a; $best = $h }
}
$found.Clear()
if ($best -ne [IntPtr]::Zero) { [void]$found.Add($best) }

if ($found.Count -eq 0) {
    Write-Output "ERROR: QEMU window not found (ready=$ready)"
    Stop-Process -Id $q.Id -Force -ErrorAction SilentlyContinue
    exit 1
}

$hw = $found[0]
$r = New-Object WinCap+RECT
[WinCap]::GetWindowRect($hw, [ref]$r) | Out-Null
$w = $r.R - $r.L; $h2 = $r.B - $r.T
$bmp = New-Object System.Drawing.Bitmap($w, $h2)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$hdc = $g.GetHdc()
[WinCap]::PrintWindow($hw, $hdc, 2) | Out-Null   # PW_RENDERFULLCONTENT
$g.ReleaseHdc($hdc); $g.Dispose()

$scale = 1200 / [double]$w
$nw = 1200; $nh = [int]($h2 * $scale)
$small = New-Object System.Drawing.Bitmap($nw, $nh)
$g2 = [System.Drawing.Graphics]::FromImage($small)
$g2.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
$g2.DrawImage($bmp, 0, 0, $nw, $nh)
$small.Save("d:\MyOS\bootloader\build\shot.png", [System.Drawing.Imaging.ImageFormat]::Png)
$g2.Dispose(); $small.Dispose(); $bmp.Dispose()

Stop-Process -Id $q.Id -Force -ErrorAction SilentlyContinue
Write-Output "ready=$ready window=${w}x${h2}"
