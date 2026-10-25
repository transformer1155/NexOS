$d = Get-PSDrive -Name C
Write-Host ("C: Used=" + [math]::Round($d.Used/1GB,2) + "GB Free=" + [math]::Round($d.Free/1GB,2) + "GB")
