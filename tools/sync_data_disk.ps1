# vbox_sync_data_disk.ps1 -- push the freshly built DATA disk into VirtualBox.
#
# WHY A SEPARATE SCRIPT
# ---------------------
# tools/vbox_sync.ps1 owns the SYSTEM disk (IDE-0-0).  Since the GGUF weights
# moved off the system image onto the data disk, the data disk (IDE-1-0) now
# carries a payload that changes independently of the kernel -- and it is a
# VDI CONTAINER in both places (build/data.vdi is written by
# tools/make_data_vdi.py, build/data_vbox.vdi is the one attached to the VM).
#
# Both are real VDI containers, so the payload cannot be copied byte-for-byte:
# the container header holds a UUID that VirtualBox keeps in its media
# registry and refuses to open if it changes.  The script therefore copies the
# file and then restores the ORIGINAL UUID, exactly like vbox_sync.ps1 does for
# the system disk.
#
# Usage (Windows host, VM powered off):
#     pwsh -NoProfile -File tools\sync_data_disk.ps1
#     pwsh -NoProfile -File tools\sync_data_disk.ps1 -WhatIf
param(
    [string]$VmName   = 'NexOS',
    [string]$RepoRoot = '',
    [switch]$WhatIf
)

$ErrorActionPreference = 'Continue'

function Find-VBoxManage {
    $candidates = @(
        'C:\Program Files\Oracle\VBoxManage.exe',
        'C:\Program Files\Oracle\VirtualBox\VBoxManage.exe'
    )
    foreach ($c in $candidates) { if (Test-Path $c) { return $c } }
    $cmd = Get-Command VBoxManage.exe -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    throw 'VBoxManage.exe not found (looked in C:\Program Files\Oracle)'
}

if (-not $RepoRoot) {
    $RepoRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
}
$src  = Join-Path $RepoRoot 'build\data_disk.raw'  # freshly built (raw)
$dst  = Join-Path $RepoRoot 'build\data_vbox.vdi'   # attached to the VM

if (-not (Test-Path $src)) { throw "missing $src -- run 'make model-pack' first" }
if (-not (Test-Path $dst)) { throw "missing $dst -- create the VM disk once, then re-run" }

$VBoxManage = Find-VBoxManage
Write-Host "[vbox] VBoxManage: $VBoxManage"
Write-Host ("[vbox] src : {0}  {1:N0} bytes  {2}" -f $src, (Get-Item $src).Length, (Get-Item $src).LastWriteTime)
Write-Host ("[vbox] dst : {0}  {1:N0} bytes  {2}" -f $dst, (Get-Item $dst).Length, (Get-Item $dst).LastWriteTime)

$running = & $VBoxManage list runningvms
if ($running -match [regex]::Escape('"' + $VmName + '"')) {
    throw "VM '$VmName' is RUNNING -- power it off first (swapping a medium in use corrupts it)"
}

# --- detach IDE-1-0 so VirtualBox lets us replace the file -----------------
$attachedPath = $null
foreach ($line in (& $VBoxManage showvminfo $VmName --machinereadable)) {
    if ($line -match '^"IDE-1-0"="(.+)"') {
        $candidate = $Matches[1].Replace('\\', '\').Trim()
        if ($candidate -ne 'none') { $attachedPath = $candidate }
    }
}
$attached = ($attachedPath -and $attachedPath -ne 'none')
Write-Host "[vbox] attached as IDE-1-0: $attached ($attachedPath)"
if ($WhatIf) {
    Write-Host '[vbox] -WhatIf: would detach, copy, restore the UUID and re-attach'
    return
}

# --- keep the medium UUID ---------------------------------------------------
$oldUuid = $null
foreach ($line in (& $VBoxManage showmediuminfo disk $dst 2>$null)) {
    if ($line -match '^UUID:\s+([0-9a-fA-F-]+)') { $oldUuid = $Matches[1]; break }
}
if (-not $oldUuid) { throw "could not read the UUID of $dst (needed to keep the VM attachment valid)" }
Write-Host "[vbox] keeping medium UUID: $oldUuid"

if ($attached) {
    & $VBoxManage storageattach $VmName --storagectl IDE --port 1 --device 0 --type hdd --medium none
    if ($LASTEXITCODE -ne 0) { throw "storageattach (detach) failed rc=$LASTEXITCODE" }
}

# --- copy and restore the UUID ---------------------------------------------
# The source is a RAW disk image, so the container is built by VBoxManage --
# the authoritative converter.  Hand-rolled VDI containers are not an option:
# VirtualBox's header layout (comment at 0x54, offBlocks at 0x154, offData at
# 0x158, disk size at 0x170) differs from the classic qemu layout, and a
# container written the old way is simply not opened.
$tmp = "$dst.new.vdi"
if (Test-Path $tmp) { Remove-Item $tmp -Force }
& $VBoxManage convertfromraw $src $tmp --format VDI 2>&1 | Out-Null
if ($LASTEXITCODE -ne 0) { throw "convertfromraw failed rc=$LASTEXITCODE" }
if (-not (Test-Path $tmp)) { throw 'convertfromraw produced no file' }
& $VBoxManage internalcommands sethduuid $tmp $oldUuid 2>&1 | Out-Null
if ($LASTEXITCODE -ne 0) { throw "sethduuid failed rc=$LASTEXITCODE (the VM would report the disk inaccessible)" }

# --- swap -------------------------------------------------------------------
$backup = "$dst.prev"
if (Test-Path $backup) { Remove-Item $backup -Force }
Move-Item -Path $dst -Destination $backup -Force
Move-Item -Path $tmp -Destination $dst -Force
Write-Host ("[vbox] data disk rebuilt: {0:N0} bytes (previous kept as {1})" -f (Get-Item $dst).Length, $backup)

& $VBoxManage storageattach $VmName --storagectl IDE --port 1 --device 0 --type hdd --medium $dst
if ($LASTEXITCODE -ne 0) {
    Move-Item -Path $dst -Destination "$dst.failed" -Force
    Move-Item -Path $backup -Destination $dst -Force
    & $VBoxManage storageattach $VmName --storagectl IDE --port 1 --device 0 --type hdd --medium $dst
    throw "storageattach (re-attach) failed rc=$LASTEXITCODE -- restored the previous container"
}
Write-Host '[vbox] done: the data disk now carries the current build'
