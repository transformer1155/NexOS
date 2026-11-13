# build_sync.ps1 -- one command: build in the Linux VM, then make the result
# visible to the VirtualBox "NexOS" VM.
#
# WHY: the VirtualBox VM boots build/os_v2_vbox.vdi, NOT build/os_v2.img.
# A bare `vm-run make` only updates the raw image, so every fix looks
# "missing" inside VirtualBox.  This wrapper enforces the build rule:
# build -> convert -> keep UUID -> re-attach.
#
# Usage (Windows host, PowerShell):
#   powershell -ExecutionPolicy Bypass -File tools\build_sync.ps1
$ErrorActionPreference = 'Continue'

Write-Host '[build] vm-run make ...'
vm-run make
if ($LASTEXITCODE -ne 0) {
    throw "build failed (rc=$LASTEXITCODE); the VDI was left untouched"
}

Write-Host '[build] syncing build/os_v2.img -> VirtualBox NexOS VM ...'
& (Join-Path $PSScriptRoot 'vbox_sync.ps1')
if ($LASTEXITCODE -ne 0) {
    throw "vbox_sync failed (rc=$LASTEXITCODE)"
}
Write-Host '[build] done: the NexOS VM disk now contains this build'
