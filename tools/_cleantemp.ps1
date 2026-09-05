$tmp = 'C:\Users\trans\AppData\Local\Temp'
$items = Get-ChildItem $tmp -File -Recurse -ErrorAction SilentlyContinue | Sort-Object Length -Descending | Select-Object -First 20
$items | ForEach-Object {
    $sz = [math]::Round($_.Length/1MB,1)
    Write-Host ($_.FullName + '  ' + $sz + 'MB')
}
# Also list swap vhdx files
$swaps = Get-ChildItem 'C:\Users\trans\AppData\Local\temp' -Filter 'swap.vhdx' -Recurse -ErrorAction SilentlyContinue
$swaps | ForEach-Object { Write-Host ('SWAP: ' + $_.FullName + '  ' + [math]::Round($_.Length/1GB,2) + 'GB') }
