$root = 'D:\MyOS'
Get-ChildItem $root -Directory | ForEach-Object {
    $sz = (Get-ChildItem $_.FullName -Recurse -File -ErrorAction SilentlyContinue | Measure-Object -Property Length -Sum).Sum
    Write-Host ($_.Name + '  ' + [math]::Round($sz/1GB,2) + 'GB')
}
$d = Get-PSDrive -Name D
Write-Host ('D: Free=' + [math]::Round($d.Free/1GB,2) + 'GB')
