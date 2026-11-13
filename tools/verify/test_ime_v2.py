#!/usr/bin/env python3
"""Reproduce the Notepad Chinese-IME bug on the CURRENT os_v2 image.
#!/usr/bin/env python3
"""Reproduce the Notepad Chinese-IME bug on the CURRENT os_v2 image.
#!/usr/bin/env python3
"""Reproduce the Notepad Chinese-IME bug on the CURRENT os_v2 image.

Boot os_v2.img (+ data disk), lock-screen login, double-click the Notepad
icon, toggle IME with Shift, type pinyin, commit with Space.  Serial
markers + staged screenshots pin down exactly where it breaks.
"""
import os, sys, time, socket, subprocess, threading
#!/usr/bin/env python3
"""Reproduce the Notepad Chinese-IME bug on the CURRENT os_v2 image.

Boot os_v2.img (+ data disk), lock-screen login, double-click the Notepad
icon, toggle IME with Shift, type pinyin, commit with Space.  Serial
markers + staged screenshots pin down exactly where it breaks.
"""
import os, sys, time, socket, subprocess, threading
#!/usr/bin/env python3
"""Reproduce the Notepad Chinese-IME bug on the CURRENT os_v2 image.

Boot os_v2.img (+ data disk), lock-screen login, double-click the Notepad
icon, toggle IME with Shift, type pinyin, commit with Space.  Serial
markers + staged screenshots pin down exactly where it breaks.
"""
import os, sys, time, socket, subprocess, threading

IMG  = "build/os_v2.img"
DATA = "build/data.vhd"
WORK = "build/os_ime2.img"
MPORT = 4521
SPORT = 4522
ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
STEP = 100

SER_LINES = []
_SER_LOCK = threading.Lock()


def wait_sock(port, timeout=45.0):
    end = time.time() + timeout
    while time.time() < end:
        try:
            return socket.create_connection(("127.0.0.1", port), timeout=0.5)
        except OSError:
            time.sleep(0.2)
    raise RuntimeError("socket not ready")


def _ser_reader(sock):
    buf = b""
    sock.settimeout(0.5)
    logf = open("build/ime2_serial.log", "ab", buffering=0)
    while True:
        try:
            data = sock.recv(65536)
        except (TimeoutError, socket.timeout):
            continue
        except OSError:
            break
        if not data:
            break
        buf += data
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            with _SER_LOCK:
                SER_LINES.append(line.decode("utf-8", "replace"))
            logf.write(line + b"\n")


def ser_contains(sub, since=0):
    with _SER_LOCK:
        return any(sub in l for l in SER_LINES[since:])


def _rel(mon, dx, dy):
    mon.sendall(f"mouse_move {dx} {dy}\n".encode())
    time.sleep(0.04)


def home(mon):
    for _ in range(16):
        _rel(mon, -STEP, -STEP)
    time.sleep(0.25)


def move_to(mon, x, y):
    home(mon)
    cx = cy = 0
    while cx < x or cy < y:
        dx = min(STEP, x - cx)
        dy = min(STEP, y - cy)
        _rel(mon, dx, dy)
        cx += dx
        cy += dy
    time.sleep(0.2)


def click(mon, x, y, dbl=False, gap=0.15):
    move_to(mon, x, y)
    mon.sendall(b"mouse_button 1\n"); time.sleep(0.06)
    mon.sendall(b"mouse_button 0\n"); time.sleep(0.06)
    if dbl:
        time.sleep(gap)
        mon.sendall(b"mouse_button 1\n"); time.sleep(0.06)
        mon.sendall(b"mouse_button 0\n"); time.sleep(0.06)


def grab(mon, path):
    ppm = path.replace(".png", ".ppm")
    if os.path.exists(ppm):
        os.remove(ppm)
    mon.sendall(f"screendump {ppm}\n".encode())
    # Wait until the PPM is fully written: header declares w/h, then we need
    # exactly w*h*3 bytes.  Re-read the header each loop in case QEMU is still
    # mid-write and the size field is incomplete.
    end = time.time() + 12.0
    ok = False
    while time.time() < end:
        if not os.path.exists(ppm):
            time.sleep(0.1); continue
        try:
            with open(ppm, "rb") as f:
                assert f.readline().strip() == b"P6"
                wh = f.readline().split()
                if len(wh) < 2:
                    time.sleep(0.1); continue
                w = int(wh[0]); h = int(wh[1])
                f.readline()  # maxval
                d = f.read()
            if len(d) >= w * h * 3:
                from PIL import Image
                Image.frombytes("RGB", (w, h), d[:w*h*3]).save(path)
                ok = True
                break
        except (OSError, AssertionError, ValueError):
            pass
        time.sleep(0.12)
    if not ok:
        print(f"WARN: grab {path} incomplete, skipping", flush=True)


def key(mon, k, gap=0.25):
    mon.sendall(f"sendkey {k}\n".encode()); time.sleep(gap)


def type_keys(mon, s, gap=0.22):
    for ch in s:
        key(mon, ch, gap)


def ser_dump(path):
    with _SER_LOCK:
        with open(path, "w") as f:
            f.write("\n".join(SER_LINES))


def main():
    os.chdir(ROOT)
    subprocess.run(["cp", IMG, WORK], check=True)
    errf = open("build/qemu_ime2.err", "wb")
    qemu = subprocess.Popen([
        "qemu-system-x86_64",
        "-drive", f"format=raw,file={WORK}",
        "-drive", f"format=raw,file={DATA}",
        "-m", "1024", "-accel", "tcg,tb-size=128",
        "-vga", "std", "-display", "none", "-no-reboot",
        "-monitor", f"tcp:127.0.0.1:{MPORT},server,nowait",
        "-serial", f"tcp:127.0.0.1:{SPORT},server,nowait",
    ], stdout=errf, stderr=errf)
    mon = None
    try:
        mon = wait_sock(MPORT); mon.settimeout(3.0)
        ser = wait_sock(SPORT)
        threading.Thread(target=_ser_reader, args=(ser,), daemon=True).start()
        try: mon.recv(65536)
        except (TimeoutError, socket.timeout): pass

        # ---- boot to lock screen (tcg is slow; be generous) ----
        # Fixed sleeps broke when boot time grew (data-disk mount probe):
        # keystrokes landed on the splash screen and the whole run went
        # dead.  Wait for the serial marker the kernel prints when the
        # managed GUI (lock screen) takes over, then settle a bit.
        print("booting...", flush=True)
        ready = False
        for _ in range(150):
            if ser_contains("mforms: entry") or ser_contains("Lock"):
                ready = True
                break
            time.sleep(1.0)
        print("boot ready:", ready, flush=True)
        time.sleep(6.0)
        grab(mon, "build/ime2_lock.png")

        # ---- login: admin + Enter ----
        type_keys(mon, "admin", gap=0.3)
        key(mon, "ret"); time.sleep(30.0)
        grab(mon, "build/ime2_desk.png")

        # ---- double-click Notepad icon at (68,65) ----
        click(mon, 68, 65, dbl=True)
        time.sleep(6.0)
        grab(mon, "build/ime2_np_open.png")
        idx = len(SER_LINES)

        # ---- Shift -> Chinese ----
        key(mon, "shift"); time.sleep(1.0)
        cn = ser_contains("IME mode -> Chinese", idx)
        if not cn:
            key(mon, "shift"); time.sleep(1.0)
            cn = ser_contains("IME mode -> Chinese", idx)
        print("shift_cn:", cn, flush=True)
        grab(mon, "build/ime2_cn_mode.png")

        # ---- type pinyin "ni" ----
        type_keys(mon, "ni"); time.sleep(1.0)
        grab(mon, "build/ime2_compose.png")

        # ---- Space -> commit ----
        key(mon, "spc"); time.sleep(1.5)
        commit = ser_contains("[IME] commit", idx)
        to_managed = ser_contains("-> managed", idx)
        print("commit:", commit, "to_managed:", to_managed, flush=True)
        grab(mon, "build/ime2_commit.png")

        # ---- continue typing "hao" and commit again ----
        type_keys(mon, "hao"); time.sleep(1.0)
        grab(mon, "build/ime2_compose2.png")
        key(mon, "spc"); time.sleep(1.5)
        grab(mon, "build/ime2_commit2.png")

        # ---- backspace after commit ----
        key(mon, "bsp"); time.sleep(1.0)
        grab(mon, "build/ime2_after_bsp.png")

        ser_dump("build/ime2_serial_full.log")
        print("SERIAL TAIL:")
        with _SER_LOCK:
            for l in SER_LINES[idx:]:
                print("  |", l)
        return 0
    finally:
        try:
            if mon: mon.sendall(b"quit\n")
        except Exception:
            pass
        qemu.terminate()
        try: qemu.wait(timeout=5)
        except Exception: qemu.kill()


if __name__ == "__main__":
    sys.exit(main())

IMG  = "build/os_v2.img"
DATA = "build/data.vhd"
WORK = "build/os_ime2.img"
MPORT = 4521
SPORT = 4522
ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
STEP = 100

SER_LINES = []
_SER_LOCK = threading.Lock()


def wait_sock(port, timeout=45.0):
    end = time.time() + timeout
    while time.time() < end:
        try:
            return socket.create_connection(("127.0.0.1", port), timeout=0.5)
        except OSError:
            time.sleep(0.2)
    raise RuntimeError("socket not ready")


def _ser_reader(sock):
    buf = b""
    sock.settimeout(0.5)
    logf = open("build/ime2_serial.log", "ab", buffering=0)
    while True:
        try:
            data = sock.recv(65536)
        except (TimeoutError, socket.timeout):
            continue
        except OSError:
            break
        if not data:
            break
        buf += data
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            with _SER_LOCK:
                SER_LINES.append(line.decode("utf-8", "replace"))
            logf.write(line + b"\n")


def ser_contains(sub, since=0):
    with _SER_LOCK:
        return any(sub in l for l in SER_LINES[since:])


def _rel(mon, dx, dy):
    mon.sendall(f"mouse_move {dx} {dy}\n".encode())
    time.sleep(0.04)


def home(mon):
    for _ in range(16):
        _rel(mon, -STEP, -STEP)
    time.sleep(0.25)


def move_to(mon, x, y):
    home(mon)
    cx = cy = 0
    while cx < x or cy < y:
        dx = min(STEP, x - cx)
        dy = min(STEP, y - cy)
        _rel(mon, dx, dy)
        cx += dx
        cy += dy
    time.sleep(0.2)


def click(mon, x, y, dbl=False, gap=0.15):
    move_to(mon, x, y)
    mon.sendall(b"mouse_button 1\n"); time.sleep(0.06)
    mon.sendall(b"mouse_button 0\n"); time.sleep(0.06)
    if dbl:
        time.sleep(gap)
        mon.sendall(b"mouse_button 1\n"); time.sleep(0.06)
        mon.sendall(b"mouse_button 0\n"); time.sleep(0.06)


def grab(mon, path):
    ppm = path.replace(".png", ".ppm")
    if os.path.exists(ppm):
        os.remove(ppm)
    mon.sendall(f"screendump {ppm}\n".encode())
    # Wait until the PPM is fully written: header declares w/h, then we need
    # exactly w*h*3 bytes.  Re-read the header each loop in case QEMU is still
    # mid-write and the size field is incomplete.
    end = time.time() + 12.0
    ok = False
    while time.time() < end:
        if not os.path.exists(ppm):
            time.sleep(0.1); continue
        try:
            with open(ppm, "rb") as f:
                assert f.readline().strip() == b"P6"
                wh = f.readline().split()
                if len(wh) < 2:
                    time.sleep(0.1); continue
                w = int(wh[0]); h = int(wh[1])
                f.readline()  # maxval
                d = f.read()
            if len(d) >= w * h * 3:
                from PIL import Image
                Image.frombytes("RGB", (w, h), d[:w*h*3]).save(path)
                ok = True
                break
        except (OSError, AssertionError, ValueError):
            pass
        time.sleep(0.12)
    if not ok:
        print(f"WARN: grab {path} incomplete, skipping", flush=True)


def key(mon, k, gap=0.25):
    mon.sendall(f"sendkey {k}\n".encode()); time.sleep(gap)


def type_keys(mon, s, gap=0.22):
    for ch in s:
        key(mon, ch, gap)


def ser_dump(path):
    with _SER_LOCK:
        with open(path, "w") as f:
            f.write("\n".join(SER_LINES))


def main():
    os.chdir(ROOT)
    subprocess.run(["cp", IMG, WORK], check=True)
    errf = open("build/qemu_ime2.err", "wb")
    qemu = subprocess.Popen([
        "qemu-system-x86_64",
        "-drive", f"format=raw,file={WORK}",
        "-drive", f"format=raw,file={DATA}",
        "-m", "1024", "-accel", "tcg,tb-size=128",
        "-vga", "std", "-display", "none", "-no-reboot",
        "-monitor", f"tcp:127.0.0.1:{MPORT},server,nowait",
        "-serial", f"tcp:127.0.0.1:{SPORT},server,nowait",
    ], stdout=errf, stderr=errf)
    mon = None
    try:
        mon = wait_sock(MPORT); mon.settimeout(3.0)
        ser = wait_sock(SPORT)
        threading.Thread(target=_ser_reader, args=(ser,), daemon=True).start()
        try: mon.recv(65536)
        except (TimeoutError, socket.timeout): pass

        # ---- boot to lock screen (tcg is slow; be generous) ----
        print("booting...", flush=True)
        time.sleep(55.0)
        grab(mon, "build/ime2_lock.png")

        # ---- login: admin + Enter ----
        type_keys(mon, "admin", gap=0.3)
        key(mon, "ret"); time.sleep(20.0)
        grab(mon, "build/ime2_desk.png")

        # ---- double-click Notepad icon at (68,65) ----
        click(mon, 68, 65, dbl=True)
        time.sleep(6.0)
        grab(mon, "build/ime2_np_open.png")
        idx = len(SER_LINES)

        # ---- Shift -> Chinese ----
        key(mon, "shift"); time.sleep(1.0)
        cn = ser_contains("IME mode -> Chinese", idx)
        if not cn:
            key(mon, "shift"); time.sleep(1.0)
            cn = ser_contains("IME mode -> Chinese", idx)
        print("shift_cn:", cn, flush=True)
        grab(mon, "build/ime2_cn_mode.png")

        # ---- type pinyin "ni" ----
        type_keys(mon, "ni"); time.sleep(1.0)
        grab(mon, "build/ime2_compose.png")

        # ---- Space -> commit ----
        key(mon, "spc"); time.sleep(1.5)
        commit = ser_contains("[IME] commit", idx)
        to_managed = ser_contains("-> managed", idx)
        print("commit:", commit, "to_managed:", to_managed, flush=True)
        grab(mon, "build/ime2_commit.png")

        # ---- continue typing "hao" and commit again ----
        type_keys(mon, "hao"); time.sleep(1.0)
        grab(mon, "build/ime2_compose2.png")
        key(mon, "spc"); time.sleep(1.5)
        grab(mon, "build/ime2_commit2.png")

        # ---- backspace after commit ----
        key(mon, "bsp"); time.sleep(1.0)
        grab(mon, "build/ime2_after_bsp.png")

        ser_dump("build/ime2_serial_full.log")
        print("SERIAL TAIL:")
        with _SER_LOCK:
            for l in SER_LINES[idx:]:
                print("  |", l)
        return 0
    finally:
        try:
            if mon: mon.sendall(b"quit\n")
        except Exception:
            pass
        qemu.terminate()
        try: qemu.wait(timeout=5)
        except Exception: qemu.kill()


if __name__ == "__main__":
    sys.exit(main())

IMG  = "build/os_v2.img"
DATA = "build/data.vhd"
WORK = "build/os_ime2.img"
MPORT = 4521
SPORT = 4522
ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
STEP = 100

SER_LINES = []
_SER_LOCK = threading.Lock()


def wait_sock(port, timeout=45.0):
    end = time.time() + timeout
    while time.time() < end:
        try:
            return socket.create_connection(("127.0.0.1", port), timeout=0.5)
        except OSError:
            time.sleep(0.2)
    raise RuntimeError("socket not ready")


def _ser_reader(sock):
    buf = b""
    sock.settimeout(0.5)
    logf = open("build/ime2_serial.log", "ab", buffering=0)
    while True:
        try:
            data = sock.recv(65536)
        except (TimeoutError, socket.timeout):
            continue
        except OSError:
            break
        if not data:
            break
        buf += data
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            with _SER_LOCK:
                SER_LINES.append(line.decode("utf-8", "replace"))
            logf.write(line + b"\n")


def ser_contains(sub, since=0):
    with _SER_LOCK:
        return any(sub in l for l in SER_LINES[since:])


def _rel(mon, dx, dy):
    mon.sendall(f"mouse_move {dx} {dy}\n".encode())
    time.sleep(0.04)


def home(mon):
    for _ in range(16):
        _rel(mon, -STEP, -STEP)
    time.sleep(0.25)


def move_to(mon, x, y):
    home(mon)
    cx = cy = 0
    while cx < x or cy < y:
        dx = min(STEP, x - cx)
        dy = min(STEP, y - cy)
        _rel(mon, dx, dy)
        cx += dx
        cy += dy
    time.sleep(0.2)


def click(mon, x, y, dbl=False, gap=0.15):
    move_to(mon, x, y)
    mon.sendall(b"mouse_button 1\n"); time.sleep(0.06)
    mon.sendall(b"mouse_button 0\n"); time.sleep(0.06)
    if dbl:
        time.sleep(gap)
        mon.sendall(b"mouse_button 1\n"); time.sleep(0.06)
        mon.sendall(b"mouse_button 0\n"); time.sleep(0.06)


def grab(mon, path):
    ppm = path.replace(".png", ".ppm")
    if os.path.exists(ppm):
        os.remove(ppm)
    mon.sendall(f"screendump {ppm}\n".encode())
    # Wait until the PPM is fully written: header declares w/h, then we need
    # exactly w*h*3 bytes.  Re-read the header each loop in case QEMU is still
    # mid-write and the size field is incomplete.
    end = time.time() + 12.0
    ok = False
    while time.time() < end:
        if not os.path.exists(ppm):
            time.sleep(0.1); continue
        try:
            with open(ppm, "rb") as f:
                assert f.readline().strip() == b"P6"
                wh = f.readline().split()
                if len(wh) < 2:
                    time.sleep(0.1); continue
                w = int(wh[0]); h = int(wh[1])
                f.readline()  # maxval
                d = f.read()
            if len(d) >= w * h * 3:
                from PIL import Image
                Image.frombytes("RGB", (w, h), d[:w*h*3]).save(path)
                ok = True
                break
        except (OSError, AssertionError, ValueError):
            pass
        time.sleep(0.12)
    if not ok:
        print(f"WARN: grab {path} incomplete, skipping", flush=True)


def key(mon, k, gap=0.25):
    mon.sendall(f"sendkey {k}\n".encode()); time.sleep(gap)


def type_keys(mon, s, gap=0.22):
    for ch in s:
        key(mon, ch, gap)


def ser_dump(path):
    with _SER_LOCK:
        with open(path, "w") as f:
            f.write("\n".join(SER_LINES))


def main():
    os.chdir(ROOT)
    subprocess.run(["cp", IMG, WORK], check=True)
    errf = open("build/qemu_ime2.err", "wb")
    qemu = subprocess.Popen([
        "qemu-system-x86_64",
        "-drive", f"format=raw,file={WORK}",
        "-drive", f"format=raw,file={DATA}",
        "-m", "1024", "-accel", "tcg,tb-size=128",
        "-vga", "std", "-display", "none", "-no-reboot",
        "-monitor", f"tcp:127.0.0.1:{MPORT},server,nowait",
        "-serial", f"tcp:127.0.0.1:{SPORT},server,nowait",
    ], stdout=errf, stderr=errf)
    mon = None
    try:
        mon = wait_sock(MPORT); mon.settimeout(3.0)
        ser = wait_sock(SPORT)
        threading.Thread(target=_ser_reader, args=(ser,), daemon=True).start()
        try: mon.recv(65536)
        except (TimeoutError, socket.timeout): pass

        # ---- boot to lock screen (tcg is slow; be generous) ----
        print("booting...", flush=True)
        time.sleep(55.0)
        grab(mon, "build/ime2_lock.png")

        # ---- login: admin + Enter ----
        type_keys(mon, "admin", gap=0.3)
        key(mon, "ret"); time.sleep(20.0)
        grab(mon, "build/ime2_desk.png")

        # ---- double-click Notepad icon at (68,65) ----
        click(mon, 68, 65, dbl=True)
        time.sleep(6.0)
        grab(mon, "build/ime2_np_open.png")
        idx = len(SER_LINES)

        # ---- Shift -> Chinese ----
        key(mon, "shift"); time.sleep(1.0)
        cn = ser_contains("IME mode -> Chinese", idx)
        if not cn:
            key(mon, "shift"); time.sleep(1.0)
            cn = ser_contains("IME mode -> Chinese", idx)
        print("shift_cn:", cn, flush=True)
        grab(mon, "build/ime2_cn_mode.png")

        # ---- type pinyin "ni" ----
        type_keys(mon, "ni"); time.sleep(1.0)
        grab(mon, "build/ime2_compose.png")

        # ---- Space -> commit ----
        key(mon, "spc"); time.sleep(1.5)
        commit = ser_contains("[IME] commit", idx)
        to_managed = ser_contains("-> managed", idx)
        print("commit:", commit, "to_managed:", to_managed, flush=True)
        grab(mon, "build/ime2_commit.png")

        # ---- continue typing "hao" and commit again ----
        type_keys(mon, "hao"); time.sleep(1.0)
        grab(mon, "build/ime2_compose2.png")
        key(mon, "spc"); time.sleep(1.5)
        grab(mon, "build/ime2_commit2.png")

        # ---- backspace after commit ----
        key(mon, "bsp"); time.sleep(1.0)
        grab(mon, "build/ime2_after_bsp.png")

        ser_dump("build/ime2_serial_full.log")
        print("SERIAL TAIL:")
        with _SER_LOCK:
            for l in SER_LINES[idx:]:
                print("  |", l)
        return 0
    finally:
        try:
            if mon: mon.sendall(b"quit\n")
        except Exception:
            pass
        qemu.terminate()
        try: qemu.wait(timeout=5)
        except Exception: qemu.kill()


if __name__ == "__main__":
    sys.exit(main())

Boot os_v2.img (+ data disk), lock-screen login, double-click the Notepad
icon, toggle IME with Shift, type pinyin, commit with Space.  Serial
markers + staged screenshots pin down exactly where it breaks.
"""
import os, sys, time, socket, subprocess, threading

IMG  = "build/os_v2.img"
DATA = "build/data.vhd"
WORK = "build/os_ime2.img"
MPORT = 4521
SPORT = 4522
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
STEP = 100

SER_LINES = []
_SER_LOCK = threading.Lock()


def wait_sock(port, timeout=30.0):
    end = time.time() + timeout
    while time.time() < end:
        try:
            return socket.create_connection(("127.0.0.1", port), timeout=0.5)
        except OSError:
            time.sleep(0.2)
    raise RuntimeError("socket not ready")


def _ser_reader(sock):
    buf = b""
    sock.settimeout(0.5)
    logf = open("build/ime2_serial.log", "ab", buffering=0)
    while True:
        try:
            data = sock.recv(65536)
        except (TimeoutError, socket.timeout):
            continue
        except OSError:
            break
        if not data:
            break
        buf += data
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            with _SER_LOCK:
                SER_LINES.append(line.decode("utf-8", "replace"))
            logf.write(line + b"\n")


def ser_contains(sub, since=0):
    with _SER_LOCK:
        return any(sub in l for l in SER_LINES[since:])


def _rel(mon, dx, dy):
    mon.sendall(f"mouse_move {dx} {dy}\n".encode())
    time.sleep(0.04)


def home(mon):
    for _ in range(16):
        _rel(mon, -STEP, -STEP)
    time.sleep(0.25)


def move_to(mon, x, y):
    home(mon)
    cx = cy = 0
    while cx < x or cy < y:
        dx = min(STEP, x - cx)
        dy = min(STEP, y - cy)
        _rel(mon, dx, dy)
        cx += dx
        cy += dy
    time.sleep(0.2)


def click(mon, x, y, dbl=False, gap=0.15):
    move_to(mon, x, y)
    mon.sendall(b"mouse_button 1\n"); time.sleep(0.06)
    mon.sendall(b"mouse_button 0\n"); time.sleep(0.06)
    if dbl:
        time.sleep(gap)
        mon.sendall(b"mouse_button 1\n"); time.sleep(0.06)
        mon.sendall(b"mouse_button 0\n"); time.sleep(0.06)


def grab(mon, path):
    ppm = path.replace(".png", ".ppm")
    if os.path.exists(ppm):
        os.remove(ppm)
    mon.sendall(f"screendump {ppm}\n".encode())
    end = time.time() + 8.0
    while time.time() < end:
        if os.path.exists(ppm) and os.path.getsize(ppm) > 1024:
            time.sleep(0.25)
            break
        time.sleep(0.12)
    from PIL import Image
    with open(ppm, "rb") as f:
        assert f.readline().strip() == b"P6"
        wh = f.readline().split()
        w = int(wh[0]); h = int(wh[1])
        f.readline()
        d = f.read()
    Image.frombytes("RGB", (w, h), d).save(path)


def key(mon, k, gap=0.25):
    mon.sendall(f"sendkey {k}\n".encode()); time.sleep(gap)


def type_keys(mon, s, gap=0.22):
    for ch in s:
        key(mon, ch, gap)


def ser_dump(path):
    with _SER_LOCK:
        with open(path, "w") as f:
            f.write("\n".join(SER_LINES))


def main():
    os.chdir(ROOT)
    subprocess.run(["cp", IMG, WORK], check=True)
    errf = open("build/qemu_ime2.err", "wb")
    qemu = subprocess.Popen([
        "qemu-system-x86_64",
        "-drive", f"format=raw,file={WORK}",
        "-drive", f"format=raw,file={DATA}",
        "-m", "256", "-accel", "tcg,tb-size=128",
        "-vga", "std", "-display", "none", "-no-reboot",
        "-monitor", f"tcp:127.0.0.1:{MPORT},server,nowait",
        "-serial", f"tcp:127.0.0.1:{SPORT},server,nowait",
    ], stdout=errf, stderr=errf)
    mon = None
    try:
        mon = wait_sock(MPORT); mon.settimeout(3.0)
        ser = wait_sock(SPORT)
        threading.Thread(target=_ser_reader, args=(ser,), daemon=True).start()
        try: mon.recv(65536)
        except (TimeoutError, socket.timeout): pass

        # ---- boot to lock screen (tcg is slow; be generous) ----
        print("booting...", flush=True)
        time.sleep(55.0)
        grab(mon, "build/ime2_lock.png")

        # ---- login: admin + Enter ----
        type_keys(mon, "admin", gap=0.3)
        key(mon, "ret"); time.sleep(20.0)
        grab(mon, "build/ime2_desk.png")

        # ---- double-click Notepad icon at (68,65) ----
        click(mon, 68, 65, dbl=True)
        time.sleep(6.0)
        grab(mon, "build/ime2_np_open.png")
        idx = len(SER_LINES)

        # ---- Shift -> Chinese ----
        key(mon, "shift"); time.sleep(1.0)
        cn = ser_contains("IME mode -> Chinese", idx)
        if not cn:
            key(mon, "shift"); time.sleep(1.0)
            cn = ser_contains("IME mode -> Chinese", idx)
        print("shift_cn:", cn, flush=True)
        grab(mon, "build/ime2_cn_mode.png")

        # ---- type pinyin "ni" ----
        type_keys(mon, "ni"); time.sleep(1.0)
        grab(mon, "build/ime2_compose.png")

        # ---- Space -> commit ----
        key(mon, "spc"); time.sleep(1.5)
        commit = ser_contains("[IME] commit", idx)
        to_managed = ser_contains("-> managed", idx)
        print("commit:", commit, "to_managed:", to_managed, flush=True)
        grab(mon, "build/ime2_commit.png")

        # ---- continue typing "hao" and commit again ----
        type_keys(mon, "hao"); time.sleep(1.0)
        grab(mon, "build/ime2_compose2.png")
        key(mon, "spc"); time.sleep(1.5)
        grab(mon, "build/ime2_commit2.png")

        # ---- backspace after commit ----
        key(mon, "bsp"); time.sleep(1.0)
        grab(mon, "build/ime2_after_bsp.png")

        ser_dump("build/ime2_serial_full.log")
        print("SERIAL TAIL:")
        with _SER_LOCK:
            for l in SER_LINES[idx:]:
                print("  |", l)
        return 0
    finally:
        try:
            if mon: mon.sendall(b"quit\n")
        except Exception:
            pass
        qemu.terminate()
        try: qemu.wait(timeout=5)
        except Exception: qemu.kill()


if __name__ == "__main__":
    sys.exit(main())

Boot os_v2.img (+ data disk), lock-screen login, double-click the Notepad
icon, toggle IME with Shift, type pinyin, commit with Space.  Serial
markers + staged screenshots pin down exactly where it breaks.
"""
import os, sys, time, socket, subprocess, threading

IMG  = "build/os_v2.img"
DATA = "build/data.vhd"
WORK = "build/os_ime2.img"
MPORT = 4521
SPORT = 4522
ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
STEP = 100

SER_LINES = []
_SER_LOCK = threading.Lock()


def wait_sock(port, timeout=45.0):
    end = time.time() + timeout
    while time.time() < end:
        try:
            return socket.create_connection(("127.0.0.1", port), timeout=0.5)
        except OSError:
            time.sleep(0.2)
    raise RuntimeError("socket not ready")


def _ser_reader(sock):
    buf = b""
    sock.settimeout(0.5)
    logf = open("build/ime2_serial.log", "ab", buffering=0)
    while True:
        try:
            data = sock.recv(65536)
        except (TimeoutError, socket.timeout):
            continue
        except OSError:
            break
        if not data:
            break
        buf += data
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            with _SER_LOCK:
                SER_LINES.append(line.decode("utf-8", "replace"))
            logf.write(line + b"\n")


def ser_contains(sub, since=0):
    with _SER_LOCK:
        return any(sub in l for l in SER_LINES[since:])


def _rel(mon, dx, dy):
    mon.sendall(f"mouse_move {dx} {dy}\n".encode())
    time.sleep(0.04)


def home(mon):
    for _ in range(16):
        _rel(mon, -STEP, -STEP)
    time.sleep(0.25)


def move_to(mon, x, y):
    home(mon)
    cx = cy = 0
    while cx < x or cy < y:
        dx = min(STEP, x - cx)
        dy = min(STEP, y - cy)
        _rel(mon, dx, dy)
        cx += dx
        cy += dy
    time.sleep(0.2)


def click(mon, x, y, dbl=False, gap=0.15):
    move_to(mon, x, y)
    mon.sendall(b"mouse_button 1\n"); time.sleep(0.06)
    mon.sendall(b"mouse_button 0\n"); time.sleep(0.06)
    if dbl:
        time.sleep(gap)
        mon.sendall(b"mouse_button 1\n"); time.sleep(0.06)
        mon.sendall(b"mouse_button 0\n"); time.sleep(0.06)


def grab(mon, path):
    ppm = path.replace(".png", ".ppm")
    if os.path.exists(ppm):
        os.remove(ppm)
    mon.sendall(f"screendump {ppm}\n".encode())
    # Wait until the PPM is fully written: header declares w/h, then we need
    # exactly w*h*3 bytes.  Re-read the header each loop in case QEMU is still
    # mid-write and the size field is incomplete.
    end = time.time() + 12.0
    ok = False
    while time.time() < end:
        if not os.path.exists(ppm):
            time.sleep(0.1); continue
        try:
            with open(ppm, "rb") as f:
                assert f.readline().strip() == b"P6"
                wh = f.readline().split()
                if len(wh) < 2:
                    time.sleep(0.1); continue
                w = int(wh[0]); h = int(wh[1])
                f.readline()  # maxval
                d = f.read()
            if len(d) >= w * h * 3:
                from PIL import Image
                Image.frombytes("RGB", (w, h), d[:w*h*3]).save(path)
                ok = True
                break
        except (OSError, AssertionError, ValueError):
            pass
        time.sleep(0.12)
    if not ok:
        print(f"WARN: grab {path} incomplete, skipping", flush=True)


def key(mon, k, gap=0.25):
    mon.sendall(f"sendkey {k}\n".encode()); time.sleep(gap)


def type_keys(mon, s, gap=0.22):
    for ch in s:
        key(mon, ch, gap)


def ser_dump(path):
    with _SER_LOCK:
        with open(path, "w") as f:
            f.write("\n".join(SER_LINES))


def main():
    os.chdir(ROOT)
    subprocess.run(["cp", IMG, WORK], check=True)
    errf = open("build/qemu_ime2.err", "wb")
    qemu = subprocess.Popen([
        "qemu-system-x86_64",
        "-drive", f"format=raw,file={WORK}",
        "-drive", f"format=raw,file={DATA}",
        "-m", "1024", "-accel", "tcg,tb-size=128",
        "-vga", "std", "-display", "none", "-no-reboot",
        "-monitor", f"tcp:127.0.0.1:{MPORT},server,nowait",
        "-serial", f"tcp:127.0.0.1:{SPORT},server,nowait",
    ], stdout=errf, stderr=errf)
    mon = None
    try:
        mon = wait_sock(MPORT); mon.settimeout(3.0)
        ser = wait_sock(SPORT)
        threading.Thread(target=_ser_reader, args=(ser,), daemon=True).start()
        try: mon.recv(65536)
        except (TimeoutError, socket.timeout): pass

        # ---- boot to lock screen (tcg is slow; be generous) ----
        # Fixed sleeps broke when boot time grew (data-disk mount probe):
        # keystrokes landed on the splash screen and the whole run went
        # dead.  Wait for the serial marker the kernel prints when the
        # managed GUI (lock screen) takes over, then settle a bit.
        print("booting...", flush=True)
        ready = False
        for _ in range(150):
            if ser_contains("mforms: entry") or ser_contains("Lock"):
                ready = True
                break
            time.sleep(1.0)
        print("boot ready:", ready, flush=True)
        time.sleep(6.0)
        grab(mon, "build/ime2_lock.png")

        # ---- login: admin + Enter ----
        type_keys(mon, "admin", gap=0.3)
        key(mon, "ret"); time.sleep(30.0)
        grab(mon, "build/ime2_desk.png")

        # ---- double-click Notepad icon at (68,65) ----
        click(mon, 68, 65, dbl=True)
        time.sleep(6.0)
        grab(mon, "build/ime2_np_open.png")
        idx = len(SER_LINES)

        # ---- Shift -> Chinese ----
        key(mon, "shift"); time.sleep(1.0)
        cn = ser_contains("IME mode -> Chinese", idx)
        if not cn:
            key(mon, "shift"); time.sleep(1.0)
            cn = ser_contains("IME mode -> Chinese", idx)
        print("shift_cn:", cn, flush=True)
        grab(mon, "build/ime2_cn_mode.png")

        # ---- type pinyin "ni" ----
        type_keys(mon, "ni"); time.sleep(1.0)
        grab(mon, "build/ime2_compose.png")

        # ---- Space -> commit ----
        key(mon, "spc"); time.sleep(1.5)
        commit = ser_contains("[IME] commit", idx)
        to_managed = ser_contains("-> managed", idx)
        print("commit:", commit, "to_managed:", to_managed, flush=True)
        grab(mon, "build/ime2_commit.png")

        # ---- continue typing "hao" and commit again ----
        type_keys(mon, "hao"); time.sleep(1.0)
        grab(mon, "build/ime2_compose2.png")
        key(mon, "spc"); time.sleep(1.5)
        grab(mon, "build/ime2_commit2.png")

        # ---- backspace after commit ----
        key(mon, "bsp"); time.sleep(1.0)
        grab(mon, "build/ime2_after_bsp.png")

        ser_dump("build/ime2_serial_full.log")
        print("SERIAL TAIL:")
        with _SER_LOCK:
            for l in SER_LINES[idx:]:
                print("  |", l)
        return 0
    finally:
        try:
            if mon: mon.sendall(b"quit\n")
        except Exception:
            pass
        qemu.terminate()
        try: qemu.wait(timeout=5)
        except Exception: qemu.kill()


if __name__ == "__main__":
    sys.exit(main())
