# Build the 32-bit NexOS kernel with the Windows-side i686-elf cross toolchain
# (WSL vhdx is locked), then patch the new kernel.bin + current sfs.img into the
# existing build/os_v2.img (layout already correct from a prior good make).
$ErrorActionPreference = "Stop"
$repo = "d:\MyOS\bootloader"
Set-Location $repo
$TC   = "C:\Users\trans\elf_tools\bin"
$CC   = "$TC\i686-elf-g++.exe"
$CCl  = "$TC\i686-elf-gcc.exe"
$LD   = "$TC\i686-elf-ld.exe"
$OC   = "$TC\i686-elf-objcopy.exe"
$AS   = "$TC\i686-elf-as.exe"
$B    = "build"

$CXXFLAGS = "-m32 -ffreestanding -fno-exceptions -fno-rtti -fno-stack-protector " +
            "-fno-pic -fno-pie -fcf-protection=none -fno-strict-aliasing " +
            "-fno-asynchronous-unwind-tables -nostdlib -Os -Wall -Wextra -fpermissive -I."

# object list mirrors Makefile $(BUILD)/kernel.elf prerequisites
$objs = @()
function Compile-Cpp($src, $obj, $extra) {
    $cmd = "$CC $CXXFLAGS $extra -c $src -o $obj 2>&1"
    Write-Host "CC $src"
    $out = cmd.exe /c $cmd
    if ($LASTEXITCODE -ne 0) { Write-Error "FAILED: $src`n$out"; exit 1 }
}
function Compile-C($src, $obj, $extra) {
    $cmd = "$CCl -x c $CXXFLAGS $extra -c $src -o $obj 2>&1"
    Write-Host "CC $src"
    $out = cmd.exe /c $cmd
    if ($LASTEXITCODE -ne 0) { Write-Error "FAILED: $src`n$out"; exit 1 }
}
function Asm($src, $obj, $fmt) {
    $cmd = "$AS -f $fmt $src -o $obj 2>&1"
    Write-Host "AS $src"
    $out = cmd.exe /c $cmd
    if ($LASTEXITCODE -ne 0) { Write-Error "FAILED: $src`n$out"; exit 1 }
}

# entry.asm is NASM (no nasm on Windows).  Assemble the GAS port instead.
# switch32to64.o is NASM but already built by a prior make; reuse it.
$cmd = "$AS _rt/entry_gas.S -o $B/entry.o 2>&1"
Write-Host "AS entry_gas.S -> entry.o"
$out = cmd.exe /c $cmd
if ($LASTEXITCODE -ne 0) { Write-Error "AS FAILED`n$out"; exit 1 }
if (!(Test-Path "$B/switch32to64.o")){ Write-Error "switch32to64.o missing (need a prior make)"; exit 1 }
Compile-Cpp "kernel.cpp"    "$B/kernel.o"       ""
Compile-C   "divdi3.c"      "$B/divdi3.o"       ""
Compile-Cpp "ai_engine.cpp" "$B/ai_engine.o"    ""
Compile-Cpp "ai_plugin.cpp" "$B/ai_plugin.o"    ""
Compile-Cpp "kb.cpp"        "$B/kb.o"           ""
Compile-Cpp "skill.cpp"     "$B/skill.o"        ""
Compile-Cpp "gguf.cpp"      "$B/gguf.o"         ""
Compile-Cpp "net.cpp"       "$B/net.o"          ""
Compile-Cpp "distnet.cpp"   "$B/distnet.o"      ""
Compile-Cpp "gui.cpp"       "$B/gui.o"          ""
Compile-C   "font_vec.c"    "$B/font_vec.o"     "-I tools/stb -I tools/vecmath"
Compile-Cpp "addrman.cpp"   "$B/addrman.o"      ""
Compile-Cpp "winloader.cpp" "$B/winloader.o"    ""
Compile-Cpp "win32.cpp"     "$B/win32.o"        ""
Compile-Cpp "linux_compat.cpp" "$B/linux_compat.o" ""
Compile-Cpp "gdt.cpp"       "$B/gdt.o"          ""
Compile-Cpp "syscall.cpp"   "$B/syscall.o"      ""
Compile-Cpp "proc.cpp"      "$B/proc.o"         ""
Compile-Cpp "vfs.cpp"       "$B/vfs.o"          ""
Compile-Cpp "perm.cpp"      "$B/perm.o"         ""
Compile-Cpp "clr.cpp"       "$B/clr.o"          ""
Compile-Cpp "mforms.cpp"    "$B/mforms.o"       "-fno-optimize-sibling-calls"

$OLIST = @("$B/entry.o","$B/switch32to64.o","$B/kernel.o","$B/divdi3.o","$B/ai_engine.o",
 "$B/ai_plugin.o","$B/kb.o","$B/skill.o","$B/gguf.o","$B/net.o","$B/distnet.o","$B/gui.o",
 "$B/font_vec.o","$B/addrman.o","$B/winloader.o","$B/win32.o","$B/linux_compat.o","$B/gdt.o",
 "$B/syscall.o","$B/proc.o","$B/vfs.o","$B/perm.o","$B/clr.o","$B/mforms.o") -join " "

$cmd = "$LD -m elf_i386 -nostdlib -T linker.ld -z noexecstack -o $B/kernel.elf $OLIST 2>&1"
Write-Host "LD kernel.elf"
$out = cmd.exe /c $cmd
if ($LASTEXITCODE -ne 0) { Write-Error "LINK FAILED`n$out"; exit 1 }

$cmd = "$OC -O binary $B/kernel.elf $B/kernel.bin 2>&1"
Write-Host "OBJCOPY kernel.bin"
$out = cmd.exe /c $cmd
if ($LASTEXITCODE -ne 0) { Write-Error "OBJCOPY FAILED`n$out"; exit 1 }

$sz = (Get-Item "$B/kernel.bin").Length
Write-Host "kernel.bin = $sz bytes"
if ($sz -gt 589824) { Write-Error "kernel.bin too big (>576KiB)"; exit 1 }

# ---- patch into existing image (boot/stage2/kernel64 layout unchanged) ----
$kbin = [System.IO.File]::ReadAllBytes("$B/kernel.bin")
$sfs  = [System.IO.File]::ReadAllBytes("$B/sfs.img")
$lf   = [System.IO.File]::ReadAllBytes("$B/linux_sfs.img")
$fs = [System.IO.File]::Open("$B/os_v2.img", [System.IO.FileMode]::Open, [System.IO.FileAccess]::ReadWrite)
$fs.Seek(33*512, [System.IO.SeekOrigin]::Begin); $fs.Write($kbin, 0, $kbin.Length)
$fs.Seek(3664*512, [System.IO.SeekOrigin]::Begin); $fs.Write($sfs, 0, $sfs.Length)
$fs.Seek(3932*512, [System.IO.SeekOrigin]::Begin); $fs.Write($lf, 0, $lf.Length)
$fs.Close()
Write-Host "patched kernel.bin + sfs.img + linux_sfs.img into build/os_v2.img"
