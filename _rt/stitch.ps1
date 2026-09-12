$ErrorActionPreference = "Continue"
$repo = "d:\MyOS\bootloader"
Set-Location $repo

$boot   = [System.IO.File]::ReadAllBytes("build\boot.bin")
$stage2 = [System.IO.File]::ReadAllBytes("build\stage2.bin")
$kern   = [System.IO.File]::ReadAllBytes("_rt\kernel.bin")
$k64    = [System.IO.File]::ReadAllBytes("build\kernel64.bin")
$sfs    = [System.IO.File]::ReadAllBytes("build\sfs.img")
$lin    = [System.IO.File]::ReadAllBytes("build\linux_sfs.img")

$LBA = 512
function Pad($arr, $toSectors) {
    $need = $toSectors * $LBA
    if ($arr.Length -ge $need) { return $arr }
    $n = New-Object byte[] $need
    [System.Array]::Copy($arr, $n, $arr.Length)
    return $n
}

# base = boot + stage2 + kernel, must reach LBA 2048
$base = New-Object System.Collections.ArrayList
[void]$base.AddRange($boot)
[void]$base.AddRange($stage2)
[void]$base.AddRange($kern)
$baseArr = $base.ToArray()
# ensure base reaches at least LBA 2048
$baseArr = Pad $baseArr 2048

$buf = New-Object System.Collections.ArrayList
[void]$buf.AddRange($baseArr)
# pad to LBA 2048
while ($buf.Count -lt 2048 * $LBA) { [void]$buf.Add(0) }
# kernel64 at LBA 2048
[void]$buf.AddRange($k64)
# pad to LBA 3664 (matches Makefile SFS_LBA and kernel SFS_ALT_LBA)
while ($buf.Count -lt 3664 * $LBA) { [void]$buf.Add(0) }
# sfs at LBA 3664
[void]$buf.AddRange($sfs)
# pad past the end of the SFS volume, then place linux_sfs
$sfsEnd = 3664 + [Math]::Ceiling($sfs.Length / 512)
while ($buf.Count -lt $sfsEnd * $LBA) { [void]$buf.Add(0) }
# linux_sfs after the main SFS volume
[void]$buf.AddRange($lin)

$out = "d:\MyOS\bootloader\_rt\boot_test.img"
[System.IO.File]::WriteAllBytes($out, $buf.ToArray())
Write-Host "wrote $out  size=$($buf.Count) bytes  ($($buf.Count/512) sectors)"
Write-Host "sfs end sector = $((3488 + [Math]::Ceiling($sfs.Length/512)))  linux end sector = $((3932 + [Math]::Ceiling($lin.Length/512)))"
