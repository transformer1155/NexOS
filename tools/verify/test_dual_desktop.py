#!/usr/bin/env python3
"""Dual-architecture Win11 desktop regression (Task #185).

Goal: confirm the 32-bit and 64-bit kernels both mount the SAME Win11
desktop -- same wallpaper, icon layout and taskbar -- so the two
architectures look identical to the user.

Flow (run in WSL; QEMU lives there):
  1. Boot the 32-bit kernel, log in (root/admin).
  2. `gui` -> Win11 desktop.  Serial marker: "[GUI] Entered GUI mode".
  3. screendump 32bit.ppm.
  4. `switch` -> 64-bit long-mode kernel.  Marker: "[K64-1] kmain64 entered".
  5. `gui` -> Win11 desktop.  Marker: "[K64] Entering Win11 GUI mode".
  6. screendump 64bit.ppm.
  7. Compare the two PPMs structurally (spatial grid + histogram) and
     assert both are non-blank and contain a bottom taskbar band.

QEMU `screendump` emits a 1280x720 PPM (P6).  We convert to PNG with a
tiny stdlib zlib encoder so the images can be presented to the user.
"""
import os, sys, time, socket, subprocess, struct, zlib, threading

IMG = "build/os.img"
WORK = "build/os_dual_test.img"
MPORT = 4491
SPORT = 4492

SER_BUF = bytearray()
_SER_LOCK = threading.Lock()


def wait_sock(port, timeout=30.0):
    end = time.time() + timeout
    while time.time() < end:
        try:
            return socket.create_connection(("127.0.0.1", port), timeout=0.5)
        except OSError:
            time.sleep(0.2)
    raise RuntimeError("socket not ready on port %d" % port)


def _ser_reader(sock):
    sock.settimeout(0.5)
    logf = open("build/serial_dual.log", "ab")
    while True:
        try:
            data = sock.recv(65536)
        except (TimeoutError, socket.timeout):
            continue
        except OSError:
            break
        if not data:
            break
        logf.write(data); logf.flush()
        with _SER_LOCK:
            SER_BUF.extend(data)
    logf.close()


def log_text():
    with _SER_LOCK:
        return SER_BUF.decode("utf-8", "replace")


def wait_for_log(needle, timeout=60.0):
    end = time.time() + timeout
    while time.time() < end:
        if needle in log_text():
            return True
        time.sleep(0.25)
    return False


def type_line(mon, s):
    keymap = {' ': 'spc', '.': 'dot', '/': 'slash', '\\': 'backslash',
              '-': 'minus', '_': 'shift-minus', ':': 'shift-semicolon'}
    for ch in s:
        key = ("shift-%s" % ch.lower()) if 'A' <= ch <= 'Z' else keymap.get(ch, ch)
        mon.sendall(("sendkey %s\n" % key).encode())
        time.sleep(0.07)
    mon.sendall(b"sendkey ret\n")
    time.sleep(0.35)


def grab(mon, path):
    if os.path.exists(path):
        os.remove(path)
    mon.sendall(("screendump %s\n" % path).encode())
    end = time.time() + 8.0
    while time.time() < end:
        if os.path.exists(path) and os.path.getsize(path) > 1024:
            time.sleep(0.25)
            return path
        time.sleep(0.12)
    raise RuntimeError("screendump never appeared: %s" % path)


def load_ppm(path):
    """Return (raw_rgb_bytes, width, height)."""
    for _ in range(40):
        try:
            f = open(path, "rb")
            if f.readline().strip() != b"P6":
                f.close(); time.sleep(0.15); continue
            dims = f.readline().split()
            w = int(dims[0]); h = int(dims[1])
            f.readline()  # maxval
            d = f.read()
            f.close()
            if len(d) < w * h * 3:
                time.sleep(0.15); continue
            return d, w, h
        except OSError:
            time.sleep(0.15)
    raise RuntimeError("could not read PPM: %s" % path)


def nonblack_in(d, w, x0, y0, x1, y1):
    n = 0
    for y in range(y0, y1):
        ro = y * w * 3
        for x in range(x0, x1):
            i = ro + x * 3
            if d[i] or d[i + 1] or d[i + 2]:
                n += 1
    return n


def taskbar_presence(d, w, h):
    """Bottom ~36px strip should have a large non-black / coloured band."""
    y0 = h - 40
    return nonblack_in(d, w, 0, y0, w, h)


def downsample_grid(d, w, h, gx=32, gy=18):
    cw = w // gx; ch = h // gy
    grid = []
    for gyi in range(gy):
        row = []
        for gxi in range(gx):
            r = g = b = 0; cnt = 0
            for yy in range(gyi * ch, (gyi + 1) * ch, 2):
                for xx in range(gxi * cw, (gxi + 1) * cw, 2):
                    i = (yy * w + xx) * 3
                    r += d[i]; g += d[i + 1]; b += d[i + 2]; cnt += 1
            row.append((r // cnt, g // cnt, b // cnt))
        grid.append(row)
    return grid


def grid_diff_cells(g1, g2, tol=60):
    """Count cells whose summed channel delta exceeds tol."""
    n = 0
    for y in range(len(g1)):
        for x in range(len(g1[0])):
            a = g1[y][x]; b = g2[y][x]
            if abs(a[0] - b[0]) + abs(a[1] - b[1]) + abs(a[2] - b[2]) > tol:
                n += 1
    return n


def histogram(d, w, h, levels=16):
    """Quantized RGB histogram (levels^3 bins)."""
    bins = levels ** 3
    hist = [0] * bins
    shift = 8 - (levels - 1).bit_length() + 1 if levels < 256 else 4
    # simplest: drop to `levels` buckets per channel via right shift
    shr = 8 - (levels - 1).bit_length()
    for i in range(0, len(d), 3):
        r = d[i] >> shr; g = d[i + 1] >> shr; b = d[i + 2] >> shr
        hist[(r * levels + g) * levels + b] += 1
    tot = sum(hist) or 1
    return [c / tot for c in hist]


def hist_intersection(h1, h2):
    return sum(min(a, b) for a, b in zip(h1, h2))


def ppm_to_png(ppm_path, png_path):
    d, w, h = load_ppm(ppm_path)
    raw = bytearray()
    stride = w * 3
    for y in range(h):
        raw.append(0)  # filter type 0
        raw.extend(d[y * stride:(y + 1) * stride])
    comp = zlib.compress(bytes(raw), 9)

    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data +
                struct.pack(">I", zlib.crc32(tag + data) & 0xffffffff))

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", comp)
    png += chunk(b"IEND", b"")
    with open(png_path, "wb") as f:
        f.write(png)
    return png_path


def main():
    for f in ("build/serial_dual.log", "build/os_dual_test.img"):
        if os.path.exists(f):
            os.remove(f)
    subprocess.run(["cp", IMG, WORK], check=True)
    errf = open("build/qemu_dual.err", "wb")
    qemu = subprocess.Popen([
        "qemu-system-x86_64", "-drive", "format=raw,file=%s" % WORK,
        "-m", "128M", "-vga", "std", "-display", "none", "-no-reboot",
        "-monitor", "tcp:127.0.0.1:%d,server,nowait" % MPORT,
        "-serial", "tcp:127.0.0.1:%d,server,nowait" % SPORT,
    ], stdout=errf, stderr=errf)
    mon = None
    try:
        mon = wait_sock(MPORT); mon.settimeout(3.0)
        ser = wait_sock(SPORT)
        threading.Thread(target=_ser_reader, args=(ser,), daemon=True).start()
        try:
            mon.recv(65536)
        except (TimeoutError, socket.timeout):
            pass

        print("[BOOT] 32-bit kernel...")
        if not wait_for_log("[K5] Hello world written", 90.0):
            print("RESULT: FAIL (32-bit boot)")
            return 1
        time.sleep(2.0)
        # login
        for attempt in range(3):
            type_line(mon, "root"); time.sleep(0.6)
            type_line(mon, "admin"); time.sleep(1.2)
            type_line(mon, "echo boot-ok")
            if wait_for_log("boot-ok", 20.0):
                break
            mon.sendall(b"sendkey ret\n"); time.sleep(1.5)
        print("[LOGIN] done")

        print("[GUI] 32-bit Win11 desktop")
        type_line(mon, "gui")
        if not wait_for_log("[GUI] Entered GUI mode", 30.0):
            print("  !! 32-bit desktop did not enter")
        time.sleep(2.5)
        p32 = grab(mon, "build/dual_32.ppm")
        d32, w32, h32 = load_ppm(p32)
        print("  32-bit frame %dx%d, non-black=%d" % (
            w32, h32, nonblack_in(d32, w32, 0, 0, w32, h32)))

        print("[EXIT-GUI] leave 32-bit desktop so shell accepts `switch`")
        mon.sendall(b"sendkey esc\n")
        time.sleep(1.5)
        wait_for_log("[GUI] Exited GUI mode", 20.0)

        print("[SWITCH] -> 64-bit long mode")
        type_line(mon, "switch")
        if not wait_for_log("[K64-1] kmain64 entered", 35.0):
            print("  !! 64-bit kernel did not come up")
            print("----- serial tail -----")
            print(log_text()[-1200:])
            try:
                with open("build/qemu_dual.err", "rb") as ef:
                    print("----- qemu stderr tail -----")
                    print(ef.read().decode("latin-1", "ignore")[-1200:])
            except OSError:
                pass
            return 1
        time.sleep(3.0)
        # 64-bit has no login gate -> go straight to desktop
        print("[GUI] 64-bit Win11 desktop")
        type_line(mon, "gui")
        if not wait_for_log("[K64] Entering Win11 GUI mode", 30.0):
            print("  !! 64-bit desktop did not enter")
            return 1
        time.sleep(8.0)   # 64-bit C# shell is an interpreter: give it more frames
        p64 = grab(mon, "build/dual_64.ppm")
        d64, w64, h64 = load_ppm(p64)
        print("  64-bit frame %dx%d, non-black=%d" % (
            w64, h64, nonblack_in(d64, w64, 0, 0, w64, h64)))

        # ---- structural comparison ----
        nb32 = nonblack_in(d32, w32, 0, 0, w32, h32)
        nb64 = nonblack_in(d64, w64, 0, 0, w64, h64)
        tb32 = taskbar_presence(d32, w32, h32)
        tb64 = taskbar_presence(d64, w64, h64)
        g32 = downsample_grid(d32, w32, h32)
        g64 = downsample_grid(d64, w64, h64)
        diff_cells = grid_diff_cells(g32, g64, tol=60)
        total_cells = len(g32) * len(g32[0])
        h1 = histogram(d32, w32, h32)
        h2 = histogram(d64, w64, h64)
        hi = hist_intersection(h1, h2)

        # emit PNGs for the user
        png32 = ppm_to_png(p32, "build/dual_32.png")
        png64 = ppm_to_png(p64, "build/dual_64.png")

        checks = {
            "k32_boot":     True,
            "k32_desktop":  nb32 > w32 * h32 * 0.3,
            "k64_boot":     True,
            "k64_desktop":  nb64 > w64 * h64 * 0.3,
            "k32_taskbar":  tb32 > w32 * 20,
            "k64_taskbar":  tb64 > w64 * 20,
            "same_res":     (w32 == w64 and h32 == h64),
            "grid_similar": diff_cells < total_cells * 0.25,
            "hist_similar": hi > 0.85,
        }
        print("\n" + "=" * 66)
        print("Dual-architecture Win11 desktop comparison")
        print("=" * 66)
        for k, v in checks.items():
            print("  [%-14s] %s" % (k, "PASS" if v else "FAIL"))
        print("\n  resolution      : %dx%d  vs  %dx%d" % (w32, h32, w64, h64))
        print("  non-black px    : %d  vs  %d" % (nb32, nb64))
        print("  taskbar band px : %d  vs  %d" % (tb32, tb64))
        print("  grid diff cells : %d / %d (%.1f%%)" % (
            diff_cells, total_cells, 100.0 * diff_cells / total_cells))
        print("  histogram inters: %.3f" % hi)
        print("  png artifacts   : %s , %s" % (png32, png64))
        ok = all(checks.values())
        print("\nRESULT:", "PASS (both architectures render the same Win11 desktop)"
              if ok else "FAIL")
        return 0 if ok else 1
    finally:
        try:
            if mon:
                mon.sendall(b"quit\n")
        except Exception:
            pass
        qemu.terminate()
        try:
            qemu.wait(timeout=5)
        except Exception:
            qemu.kill()
        # belt-and-suspenders
        subprocess.run(["pkill", "-f", "[q]emu-system"], check=False)
    errf.close()


if __name__ == "__main__":
    sys.exit(main())
