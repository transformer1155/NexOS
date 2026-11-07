# interact.ps1 -- drive the guest from the outside and PROVE each step changed
# the screen.  Verdicts use tools/shot_diff.py (pixel comparison, HUD excluded),
# never file sizes: the HUD clock changes every second so sizes always wobble.
param(
    [string]$VmName = 'NexOS',
    $Steps = '',          # untyped on purpose; [string] would join arrays with spaces
    [string]$ShotDir = 'build\shots',
    [int]$SettleMs = 900
)
$ErrorActionPreference = 'Stop'
$vbm = 'C:\Program Files\Oracle\VBoxManage.exe'
$repo = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)

$SC = @{
    'esc'=@('01'); 'win'=@('e0','5b'); 'super'=@('e0','5b')
    'enter'=@('1c'); 'space'=@('39'); 'tab'=@('0f'); 'bksp'=@('0e')
    'up'=@('e0','48'); 'down'=@('e0','50'); 'left'=@('e0','4b'); 'right'=@('e0','4d')
    'pgup'=@('e0','49'); 'pgdn'=@('e0','51'); 'home'=@('e0','47'); 'end'=@('e0','4f')
    'del'=@('e0','53')
    'a'=@('1e');'b'=@('30');'c'=@('2e');'d'=@('20');'e'=@('12');'f'=@('21')
    'g'=@('22');'h'=@('23');'i'=@('17');'j'=@('24');'k'=@('25');'l'=@('26')
    'm'=@('32');'n'=@('31');'o'=@('18');'p'=@('19');'q'=@('10');'r'=@('13')
    's'=@('1f');'t'=@('14');'u'=@('16');'v'=@('2f');'w'=@('11');'x'=@('2d')
    'y'=@('15');'z'=@('2c')
    '1'=@('02');'2'=@('03');'3'=@('04');'4'=@('05');'5'=@('06')
    '6'=@('07');'7'=@('08');'8'=@('09');'9'=@('0a');'0'=@('0b')
}

function Key-Seq([string]$name) {
    $n = $name.ToLower()
    if (-not $SC.ContainsKey($n)) { throw "unknown key '$name'" }
    $mk = $SC[$n]
    $ext = $false
    if ($mk[0] -eq 'e0') { $ext = $true; $mk = @($mk[1]) }
    $b = [Convert]::ToByte($mk[0],16)
    $brk = '{0:x2}' -f (($b -bor 0x80) -band 0xFF)
    if ($ext) { return @('e0', $mk[0], 'e0', $brk) }
    return @($mk[0], $brk)
}

# Invoke VBoxManage with the scancodes as SEPARATE arguments.
#
# The naive `& $vbm controlvm $VmName keyboardputscancode @(Key-Seq $k)` failed
# silently: the array got stringified into one space-separated argument, VBoxManage
# rejected it, and every step looked like "the input did not reach the guest" --
# which is exactly the wrong conclusion.  Splatting a flattened string array and
# checking $LASTEXITCODE makes a failed injection visible instead of silent.
function Inject([string]$keyName) {
    $seq = @(Key-Seq $keyName)
    $flat = $seq -join ' '
    if ($flat -eq '') { throw "empty scancode sequence for '$keyName'" }
    $args = @('controlvm', $VmName, 'keyboardputscancode') + $seq
    $out = & $vbm @args 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "keyboardputscancode failed for '$keyName' (rc=$LASTEXITCODE): $out"
    }
    return $true
}

function Shot([string]$path) {
    Remove-Item $path -Force -ErrorAction SilentlyContinue
    & $vbm controlvm $VmName screenshotpng $path 2>$null | Out-Null
    if (-not (Test-Path $path)) { throw "screenshot failed" }
}

function Diff([string]$a, [string]$b) {
    $out = & python "$repo\tools\shot_diff.py" $a $b 2>&1
    $rc = $LASTEXITCODE
    return @{ rc = $rc; text = ($out -join "`n") }
}

if (-not (Test-Path $ShotDir)) { New-Item -ItemType Directory -Path $ShotDir | Out-Null }

$flat = if ($Steps -is [array]) { $Steps -join ',' } else { [string]$Steps }
if ($flat.Trim() -eq '') { $steps = @('win','esc','win','down','down','enter','esc') }
else { $steps = [regex]::Split($flat, '[,\s]+') | Where-Object { $_ -ne '' } }

$rep = Join-Path $ShotDir 'report.txt'
"interaction report $(Get-Date -Format o)" | Out-File $rep

$prev = Join-Path $ShotDir 'step0.png'
Shot $prev
"  boot        captured" | Out-File $rep -Append

$i = 0; $fail = 0
foreach ($k in $steps) {
    $i++
    Inject $k | Out-Null
    Start-Sleep -Milliseconds $SettleMs
    $cur = Join-Path $ShotDir ("step{0}.png" -f $i)
    Shot $cur
    $d = Diff $prev $cur
    if ($d.rc -eq 0) { $v = 'CHANGED' } else { $v = 'NO-OP  <-- interaction bug'; $fail++ }
    ($d.text -split "`n" | Where-Object { $_ -match 'differing|RESULT' }) | ForEach-Object { "      $_" | Out-File $rep -Append }
    "$('{0,-6}' -f $k) $v" | Out-File $rep -Append
    $prev = $cur
}
"" | Out-File $rep -Append
"steps=$($steps.Count)  no-op=$fail" | Out-File $rep -Append
Get-Content $rep
if ($fail -gt 0) { exit 1 } else { exit 0 }
