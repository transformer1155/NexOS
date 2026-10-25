Add-Type -ReferencedAssemblies System.Drawing @"
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.IO;
using System.Runtime.InteropServices;
public class Ppm {
    public static void ToPng(string ppm, string png) {
        byte[] all = File.ReadAllBytes(ppm);
        int p = 0;
        while (all[p] != (byte)'\n') p++;
        p++;
        var toks = new System.Collections.Generic.List<string>();
        for (int i = 0; i < 3; i++) {
            while (char.IsWhiteSpace((char)all[p])) p++;
            int s = p;
            while (!char.IsWhiteSpace((char)all[p])) p++;
            toks.Add(System.Text.Encoding.ASCII.GetString(all, s, p - s));
        }
        p++;
        int w = int.Parse(toks[0]), h = int.Parse(toks[1]);
        Bitmap bmp = new Bitmap(w, h, PixelFormat.Format24bppRgb);
        Rectangle r = new Rectangle(0, 0, w, h);
        BitmapData bd = bmp.LockBits(r, ImageLockMode.WriteOnly, PixelFormat.Format24bppRgb);
        byte[] row = new byte[w * 3];
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                int si = p + (y * w + x) * 3;
                row[x * 3 + 0] = all[si + 2];
                row[x * 3 + 1] = all[si + 1];
                row[x * 3 + 2] = all[si + 0];
            }
            Marshal.Copy(row, 0, bd.Scan0 + (h - 1 - y) * bd.Stride, w * 3);
        }
        bmp.UnlockBits(bd);
        bmp.Save(png, ImageFormat.Png);
        bmp.Dispose();
    }
}
"@

$ppm = "D:/MyOS/bootloader/build/dump.ppm"
$png = "d:\MyOS\bootloader\build\dump.png"
$log = "d:\MyOS\bootloader\build\ser_shot.log"
if (Test-Path $log)   { Remove-Item $log -Force }
if (Test-Path $png)   { Remove-Item $png -Force }
if (Test-Path "d:\MyOS\bootloader\build\dump.ppm") { Remove-Item "d:\MyOS\bootloader\build\dump.ppm" -Force }

$q = Start-Process -FilePath "D:\qemu\qemu-system-x86_64.exe" -ArgumentList @(
  "-m","512","-vga","cirrus","-display","gtk","-accel","tcg",
  "-drive","file=d:\MyOS\bootloader\build\os_v2.img,format=raw",
  "-serial","file:$log",
  "-monitor","telnet:127.0.0.1:4445,server,nowait"
) -PassThru

$deadline = (Get-Date).AddSeconds(180)
$ready = $false
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 1500
    if (Test-Path $log) {
        $c = Get-Content $log -Raw -ErrorAction SilentlyContinue
        if ($c -and $c.Contains("Cirrus HW cursor enabled")) { $ready = $true; break }
    }
    if ($q.HasExited) { break }
}
if ($ready) { Start-Sleep -Seconds 10 }

try {
    $tcp = New-Object System.Net.Sockets.TcpClient("127.0.0.1", 4445)
    $st  = $tcp.GetStream()
    Start-Sleep -Milliseconds 500
    $cmd = [Text.Encoding]::ASCII.GetBytes("screendump $ppm`n")
    $st.Write($cmd, 0, $cmd.Length)
    Start-Sleep -Seconds 4
    $tcp.Close()
} catch {
    Write-Output "monitor error: $($_.Exception.Message)"
}

Stop-Process -Id $q.Id -Force -ErrorAction SilentlyContinue

if (Test-Path "d:\MyOS\bootloader\build\dump.ppm") {
    [Ppm]::ToPng("d:\MyOS\bootloader\build\dump.ppm", $png)
    Write-Output "ready=$ready dumped -> $png"
} else {
    Write-Output "ready=$ready NO PPM"
}
