cd D:\MyOS\bootloader
$blobs = git cat-file --batch-all-objects --batch-check 2>$null
$found = $false
foreach ($line in $blobs) {
  $parts = $line -split ' '
  if ($parts.Count -ge 3 -and $parts[1] -eq 'blob') {
    $size = [int]$parts[2]
    if ($size -gt 5000000 -and $size -lt 7000000) {
      $h = $parts[0]
      $tmp = "build/_scan_$h.img"
      git cat-file -p $h > $tmp 2>$null
      $hit = Select-String -Quiet -Pattern 'serial init done' $tmp
      if ($hit) {
        Write-Output "FOUND $h size=$size"
        Copy-Item $tmp build/os_v2_found.img -Force
        Write-Output "COPIED to build/os_v2_found.img"
        $found = $true
        break
      }
      Remove-Item $tmp -Force -ErrorAction SilentlyContinue
    }
  }
}
if (-not $found) { Write-Output "NO_SERIAL_INIT_IMAGE_BLOB" }
Write-Output "DONE"
