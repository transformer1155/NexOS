# NexOS QEMU Launcher (Auto Memory Detection) with Ops Console mode
# Save as: run_nexos.ps1
#
# Usage:
#   .\run_nexos.ps1            Launch with the software (std VGA) backend.
#   .\run_nexos.ps1 -GL        Launch with the OpenGL-backed display.
#                              The guest still uses the standard VGA/BGA device
#                              (so the OS framebuffer path stays unchanged and
#                              compatible), while QEMU composites the window
#                              through the host GPU via GTK/EGL OpenGL.
#   .\run_nexos.ps1 -GL -headless   OpenGL backend, no window (off-screen GL).
#   .\run_nexos.ps1 -Ops        Headless serial on tcp:127.0.0.1:4321 and
#                              auto-start the WebSocket bridge (ws:8765) so the
#                              browser ops console can connect.

Write-Host "========================================" -ForegroundColor Cyan
Write-Host "       NexOS QEMU Launcher" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan
Write-Host ""

# Parse arguments
$USE_GL      = $false
$HEADLESS_GL = $false
$OPS_MODE    = $false
$FABRIC_MODE = $false
foreach ($a in $args) {
    if ($a -eq "-GL")        { $USE_GL = $true }
    if ($a -eq "-headless")  { $HEADLESS_GL = $true }
    if ($a -eq "-Ops")       { $OPS_MODE = $true }
    if ($a -eq "-Fabric")    { $FABRIC_MODE = $true }
}
if ($USE_GL -and $HEADLESS_GL) { $USE_GL = $true }

# Configuration
$QEMU_PATH  = "D:\qemu"
$IMG_PATH   = "D:\MyOS\bootloader\build\os_v2.img"
$BRIDGE_DIR = "D:\MyOS\bootloader\tools"
$SERIAL_PORT = 4321
$WS_PORT     = 8765
$MIN_MEM = 512
$MAX_MEM = 4096

# Check QEMU
$QEMU_EXE = Join-Path $QEMU_PATH "qemu-system-x86_64.exe"
if (-not (Test-Path $QEMU_EXE)) {
    Write-Host "[ERROR] QEMU not found: $QEMU_EXE" -ForegroundColor Red
    Read-Host "Press Enter to exit"
    exit 1
}

# Check image
if (-not (Test-Path $IMG_PATH)) {
    Write-Host "[ERROR] Image not found: $IMG_PATH" -ForegroundColor Red
    Read-Host "Press Enter to exit"
    exit 1
}

Write-Host "[INFO] QEMU: $QEMU_EXE" -ForegroundColor Yellow
Write-Host "[INFO] Image: $IMG_PATH" -ForegroundColor Yellow
if ($OPS_MODE) {
    Write-Host "[INFO] Mode: Ops console (headless, serial TCP + WS bridge)" -ForegroundColor Cyan
} elseif ($USE_GL) {
    if ($HEADLESS_GL) {
        Write-Host "[INFO] Render backend: OpenGL (EGL headless, off-screen GL)" -ForegroundColor Green
    } else {
        Write-Host "[INFO] Render backend: OpenGL (virtio-gpu + GTK GL window)" -ForegroundColor Green
    }
} else {
    Write-Host "[INFO] Render backend: software (std VGA)" -ForegroundColor Yellow
}
Write-Host ""

# Get free memory
Write-Host "[Step 1/3] Detecting system memory..." -ForegroundColor Yellow

$FREE_MEM_KB = 0

try {
    $result = wmic os get FreePhysicalMemory 2>$null
    if ($LASTEXITCODE -eq 0) {
        $lines = $result -split "`r`n" | Where-Object { $_ -match "^\d+$" }
        if ($lines) {
            $FREE_MEM_KB = [int]$lines[0]
        }
    }
} catch {}

if ($FREE_MEM_KB -eq 0) {
    try {
        $mem = Get-CimInstance -ClassName Win32_OperatingSystem 2>$null
        if ($mem) {
            $FREE_MEM_KB = [int]($mem.FreePhysicalMemory)
        }
    } catch {}
}

if ($FREE_MEM_KB -eq 0) {
    Write-Host "[WARN] Cannot get memory info, use default 2048 MB" -ForegroundColor Yellow
    $ALLOC_MEM = 2048
} else {
    $FREE_MEM_GB = [math]::Round($FREE_MEM_KB / 1048576, 1)
    $FREE_MEM_MB = [int]($FREE_MEM_KB / 1024)
    Write-Host "  Free memory: $FREE_MEM_MB MB (about $FREE_MEM_GB GB)" -ForegroundColor Green

    $RESERVED_MB = 1536
    $ALLOC_MEM = $FREE_MEM_MB - $RESERVED_MB

    if ($ALLOC_MEM -lt $MIN_MEM) { $ALLOC_MEM = $MIN_MEM }
    if ($ALLOC_MEM -gt $MAX_MEM) { $ALLOC_MEM = $MAX_MEM }

    Write-Host "  Allocate VM: $ALLOC_MEM MB" -ForegroundColor Green
}

Write-Host ""

# Memory optimization
Write-Host "[Step 2/3] Memory optimization..." -ForegroundColor Yellow

[System.GC]::Collect()
[System.GC]::WaitForPendingFinalizers()
[System.GC]::Collect()
Write-Host "  GC done" -ForegroundColor Green

$isAdmin = ([Security.Principal.WindowsPrincipal] [Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)

if ($isAdmin) {
    Write-Host "  [Admin] Clearing Standby List..." -ForegroundColor Yellow
    try {
        Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public class Mem {
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern bool SetProcessWorkingSetSize(IntPtr hProcess, int dwMinimumWorkingSetSize, int dwMaximumWorkingSetSize);
}
'@ -ErrorAction SilentlyContinue
        $proc = Get-Process -Id $pid
        [Mem]::SetProcessWorkingSetSize($proc.Handle, -1, -1) | Out-Null
        Write-Host "  Standby List cleared" -ForegroundColor Green
    } catch {
        Write-Host "  Standby List clear failed" -ForegroundColor Yellow
    }
} else {
    Write-Host "  [SKIP] Non-admin mode, skip deep clean" -ForegroundColor Yellow
}

Write-Host ""

# Launch QEMU
Write-Host "[Step 3/3] Starting QEMU..." -ForegroundColor Yellow
Write-Host "  Allocate: $ALLOC_MEM MB" -ForegroundColor Green
Write-Host ""

function Start-NexOS {
    param(
        [string]$DisplayArg,
        [string]$VgaArg,
        [int]$Mem,
        [array]$SerialArg = @('-serial', 'mon:stdio')
    )
    & $QEMU_EXE -drive format=raw,file="$IMG_PATH" -m $Mem -vga $VgaArg -display $DisplayArg -machine pc,mem-merge=off @SerialArg -accel tcg,tb-size=32 -no-reboot
    return $LASTEXITCODE
}

$bridgeJob = $null

if ($USE_GL) {
    if ($HEADLESS_GL) {
        $DISPLAY_ARG = "egl-headless,gl=on"
    } else {
        $DISPLAY_ARG = "gtk,gl=on"
    }
    $VGA_ARG = "std"
    $code = Start-NexOS -DisplayArg $DISPLAY_ARG -VgaArg $VGA_ARG -Mem $ALLOC_MEM
    if ($code -ne 0) {
        Write-Host ""
        Write-Host "[ERROR] OpenGL backend failed (code: $code)" -ForegroundColor Red
        Write-Host "Falling back to software (std VGA)..." -ForegroundColor Yellow
        $code = Start-NexOS -DisplayArg "gtk" -VgaArg "std" -Mem $ALLOC_MEM
    }
} elseif ($OPS_MODE) {
    Write-Host ""
    Write-Host "[OPS MODE] NexOS serial on tcp://127.0.0.1:$SERIAL_PORT" -ForegroundColor Cyan
    Write-Host "           WebSocket bridge on ws://127.0.0.1:$WS_PORT" -ForegroundColor Cyan
    Write-Host "           Open win11-ui/nexos-desktop.html and connect." -ForegroundColor Cyan
    Write-Host ""

    # 分布式算力网络：两个真实 VM 通过 L2 hub (nexos_l2hub.py) 共享同一广播域，
    # 调度器 VM 经桥被浏览器驱动，计算节点 VM 运行 distnet compute 真实执行任务并回传结果。
    # 注意：QEMU 点对点 socket 不洪泛广播帧，SLIRP 也不转发广播，所以必须由 hub 洪泛，
    # 否则 distnet 的 QUERY 广播到不了对端 VM。两个 VM 的 MAC 必须不同。
    $FABRIC_HUB_PORT = 4322
    if ($FABRIC_MODE) {
        $hubPath = Join-Path $BRIDGE_DIR "nexos_l2hub.py"
        if (Test-Path $hubPath) {
            $hubJob = Start-Process -FilePath "python" -ArgumentList "`"$hubPath`" 127.0.0.1 $FABRIC_HUB_PORT" -WindowStyle Hidden -PassThru
            Write-Host "[INFO] Started L2 hub (PID: $($hubJob.Id)) on 127.0.0.1:$FABRIC_HUB_PORT" -ForegroundColor Green
            Start-Sleep -Seconds 1
        } else {
            Write-Host "[WARN] L2 hub not found: $hubPath" -ForegroundColor Yellow
            Write-Host "       Start it manually: python $hubPath" -ForegroundColor Yellow
        }
        $NET_ARGS = "-netdev socket,id=n0,connect=127.0.0.1:$FABRIC_HUB_PORT -device ne2k_isa,iobase=0x300,irq=3,mac=52:54:00:12:34:56,netdev=n0"
    } else {
        # NexOS 内核只实现了 NE2000 ISA 驱动 (I/O 0x300 轮询)，不支持 virtio-net，
        # 必须用 ne2k_isa，否则内核检测不到网卡（MAC 会是全 FF，网络功能全部失效）。
        $NET_ARGS = "-netdev user,id=n0,net=10.0.2.0/24 -device ne2k_isa,iobase=0x300,irq=3,netdev=n0"
    }

    $bridgePath = Join-Path $BRIDGE_DIR "nexos_bridge.py"
    if (Test-Path $bridgePath) {
        $bridgeJob = Start-Process -FilePath "python" -ArgumentList "`"$bridgePath`"" -WindowStyle Hidden -PassThru
        Write-Host "[INFO] Started bridge (PID: $($bridgeJob.Id))" -ForegroundColor Green
    } else {
        Write-Host "[WARN] Bridge script not found: $bridgePath" -ForegroundColor Yellow
        Write-Host "       Start it manually: python $bridgePath" -ForegroundColor Yellow
    }

    Start-Sleep -Seconds 1
    # Headless ops kernel needs little RAM; keep low to avoid TCG JIT buffer allocation failure
    $OPS_MEM = 128
    # 调度器 VM 后台启动（与桥/计算节点并行），自定义内核不读 -append，故省略
    $schedArgs = @('-drive', "format=raw,file=$IMG_PATH", '-m', $OPS_MEM, '-vga', 'std', '-display', 'none',
                    '-machine', 'pc,mem-merge=off',
                    '-chardev', "socket,id=ser0,server=on,wait=off,host=127.0.0.1,port=$SERIAL_PORT",
                    '-serial', 'chardev:ser0', '-accel', 'tcg,tb-size=32', '-no-reboot') + $NET_ARGS.Split(' ')
    $schedJob = Start-Process -FilePath $QEMU_EXE -ArgumentList $schedArgs -PassThru
    Write-Host "[INFO] Started scheduler VM (PID: $($schedJob.Id))" -ForegroundColor Green

    if ($FABRIC_MODE) {
        # 计算节点 VM 连到同一 L2 hub，并用串口驱动脚本登录后持续运行 distnet compute。
        # 顺序很重要：QEMU chardev socket 同时只接受一个连接，断开后不再重监听，
        # 所以必须先等计算节点 VM 的串口真正 LISTENING，再启动驱动（否则驱动会
        # 连上未就绪的端口被关掉，之后一直 ECONNREFUSED）。
        $COMPUTE_PORT = 4323
        $computeArgs = @('-drive', "format=raw,file=$IMG_PATH", '-m', '128', '-vga', 'std', '-display', 'none',
                         '-machine', 'pc,mem-merge=off',
                         '-chardev', "socket,id=ser0,server=on,wait=off,host=127.0.0.1,port=$COMPUTE_PORT",
                         '-serial', 'chardev:ser0', '-accel', 'tcg,tb-size=32', '-no-reboot',
                         '-netdev', 'socket,id=n0,connect=127.0.0.1:4322',
                         '-device', 'ne2k_isa,iobase=0x300,irq=3,mac=52:54:00:12:34:57,netdev=n0')
        $computeOut = Join-Path (Get-Location) "compute_vm.out.log"
        $computeErr = Join-Path (Get-Location) "compute_vm.err.log"
        $computeVmJob = Start-Process -FilePath $QEMU_EXE -ArgumentList $computeArgs -PassThru -RedirectStandardOutput $computeOut -RedirectStandardError $computeErr
        Write-Host "[INFO] Started compute-node VM (PID: $($computeVmJob.Id)), log: $computeOut / $computeErr" -ForegroundColor Green

        # 等待计算节点串口就绪（最多 20 秒）
        $ready = $false
        for ($i = 0; $i -lt 20; $i++) {
            Start-Sleep -Seconds 1
            $c = Get-NetTCPConnection -LocalPort $COMPUTE_PORT -State Listen -ErrorAction SilentlyContinue
            if ($c) { $ready = $true; break }
        }
        if (-not $ready) {
            Write-Host "[WARN] Compute VM serial :$COMPUTE_PORT not listening yet; driver may retry." -ForegroundColor Yellow
        }

        $driverPath = Join-Path $BRIDGE_DIR "distnet_compute_driver.py"
        if (Test-Path $driverPath) {
            # 注意：驱动独占计算节点串口，验证时不要再用其它客户端连 $COMPUTE_PORT。
            # Start-Process 要求 stdout / stderr 重定向到不同文件
            $driverLog  = Join-Path (Get-Location) "compute_serial.log"
            $driverErr  = Join-Path (Get-Location) "compute_serial.err.log"
            $computeJob = Start-Process -FilePath "python" -ArgumentList "`"$driverPath`" 127.0.0.1 $COMPUTE_PORT" -WindowStyle Hidden -PassThru -RedirectStandardOutput $driverLog -RedirectStandardError $driverErr
            Write-Host "[INFO] Started compute-node VM driver (PID: $($computeJob.Id)), log: $driverLog" -ForegroundColor Green
        } else {
            Write-Host "[WARN] Compute driver not found: $driverPath" -ForegroundColor Yellow
        }
    }

    Write-Host "[INFO] Fabric running. Scheduler VM serial on :$SERIAL_PORT, bridge on ws://127.0.0.1:$WS_PORT" -ForegroundColor Cyan
    Write-Host "[INFO] Open win11-ui/nexos-desktop.html and Connect." -ForegroundColor Cyan
    Write-Host "[INFO] Press Ctrl-C in this window to stop (VMs are detached and keep running otherwise)." -ForegroundColor Yellow
} else {
    $code = Start-NexOS -DisplayArg "gtk" -VgaArg "std" -Mem $ALLOC_MEM
    if ($code -ne 0) {
        Write-Host ""
        Write-Host "[ERROR] QEMU failed (code: $code)" -ForegroundColor Red
        Write-Host ""
        Write-Host "Retry with 512 MB..." -ForegroundColor Yellow
        $code = Start-NexOS -DisplayArg "gtk" -VgaArg "std" -Mem 512
        if ($code -ne 0) {
            Write-Host "[ERROR] 512 MB also failed" -ForegroundColor Red
            Read-Host "Press Enter to exit"
            exit 1
        }
    }
}

Write-Host ""
Write-Host "[DONE] QEMU exited" -ForegroundColor Green
Read-Host "Press Enter to exit"
