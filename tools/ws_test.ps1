$ErrorActionPreference = 'SilentlyContinue'
$ws = New-Object System.Net.WebSockets.ClientWebSocket
$ct = New-Object System.Threading.CancellationToken
$ws.ConnectAsync('ws://127.0.0.1:8765', $ct).Wait()
$buf = New-Object byte[] 8192
$all = ''
$recvBuf = New-Object System.Collections.Generic.List[byte]
function SendStr($s){ $b=[System.Text.Encoding]::UTF8.GetBytes($s); $ws.SendAsync($b, [System.Net.WebSockets.WebSocketMessageType]::Text, $true, $ct).Wait() }
function Drain($ms){ Start-Sleep -Milliseconds $ms; $acc=''
  while($ws.State -eq [System.Net.WebSockets.WebSocketState]::Open){
    try { $res=$ws.ReceiveAsync($buf,$ct).Result } catch { break }
    if($res.Count -le 0){ break }
    $acc += [System.Text.Encoding]::UTF8.GetString($buf,0,$res.Count)
  }
  if($acc){ $script:all += $acc; return $acc } return ''
}
SendStr('login nexos nexos'); Drain 800 | Out-Null
foreach($cmd in @('help','ls','ps','pwd')){
  SendStr($cmd); $r = Drain 1300
  Write-Host "===== $cmd ====="
  Write-Host $r
}
Write-Host "===== FULL LOG (last 1500 chars) ====="
Write-Host $all.Substring([Math]::Max(0,$all.Length-1500))
try { $ws.CloseAsync([System.Net.WebSockets.WebSocketCloseStatus]::NormalClosure,'bye',$ct).Wait() } catch {}
