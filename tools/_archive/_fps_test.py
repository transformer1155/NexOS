"""External frame-rate probe for the NexOS compositor.

Drives the VM ONLY through QEMU's external interfaces (QMP input-send-event /
screendump / serial log) -- no in-guest hooks, no internal APIs.  The kernel
prints "[FPS] n fps" at most once per second; we collect those lines per phase.

  phase A : idle            (no input -- measures the event-driven baseline)
  phase B : sustained mouse movement (measures interactive smoothness/jank)
"""
import subprocess, socket, json, time, os, re

QEMU     = r"D:\qemu\qemu-system-x86_64.exe"
IMG      = r"D:\MyOS\bootloader\build\os_v2.img"
OUT      = r"D:\MyOS\bootloader\build"
QMP_PORT = 4465
SERLOG   = os.path.join(OUT, "fps_serial.log")


class Q:
    def __init__(self, sock):
        self.s = sock

    def cmd(self, c, a=None):
        obj = {"execute": c}
        if a is not None:
            obj["arguments"] = a
        self.s.sendall((json.dumps(obj) + "\r\n").encode())
        buf = b""
        while b'"return"' not in buf and b'"error"' not in buf:
            d = self.s.recv(4096)
            if not d:
                break
            buf += d
        return buf.decode(errors="replace")

    def rel(self, dx, dy):
        evs = []
        if dx:
            evs.append({"type": "rel", "data": {"axis": "x", "value": dx}})
        if dy:
            evs.append({"type": "rel", "data": {"axis": "y", "value": dy}})
        for e in evs:
            self.cmd("input-send-event", {"events": [e]})
            time.sleep(0.09)          # PS/2 is a 1-byte buffer; drain between events

    def key(self, qc):
        self.cmd("input-send-event", {"events": [
            {"type": "key", "data": {"key": {"type": "qcode", "data": qc}, "down": True}},
            {"type": "key", "data": {"key": {"type": "qcode", "data": qc}, "down": False}}]})
        time.sleep(0.03)


def wait_gui(timeout=40):
    t = time.time()
    while time.time() - t < timeout:
        try:
            with open(SERLOG, "r", errors="replace") as f:
                if "Entered GUI mode" in f.read():
                    return True
        except FileNotFoundError:
            pass
        time.sleep(0.5)
    return False


def serial_tail(offset):
    """Return (new_text_since_offset, new_offset)."""
    try:
        with open(SERLOG, "rb") as f:
            f.seek(offset)
            data = f.read()
            return data.decode(errors="replace"), offset + len(data)
    except FileNotFoundError:
        return "", offset


def fps_lines(text):
    return [int(m) for m in re.findall(r"\[FPS\]\s+(\d+)\s+fps", text)]


def report(name, vals):
    if not vals:
        print("  %-22s : no samples (compositor idle / no frames)" % name)
        return
    vals_s = sorted(vals)
    med = vals_s[len(vals_s) // 2]
    print("  %-22s : n=%2d  min=%3d  median=%3d  max=%3d  samples=%s"
          % (name, len(vals), vals_s[0], med, vals_s[-1], vals))


def main():
    try:
        os.remove(SERLOG)
    except FileNotFoundError:
        pass
    args = [QEMU, "-drive", "format=raw,file=" + IMG, "-m", "512", "-vga", "std",
            "-display", "none", "-machine", "pc,mem-merge=off",
            "-accel", "tcg,tb-size=32", "-no-reboot",
            "-qmp", "tcp:127.0.0.1:%d,server,nowait" % QMP_PORT,
            "-serial", "file:" + SERLOG]
    p = subprocess.Popen(args)
    try:
        print("[*] GUI up:", wait_gui())
        time.sleep(2)
        s = socket.create_connection(("127.0.0.1", QMP_PORT))
        s.recv(4096)
        q = Q(s)
        q.cmd("qmp_capabilities")
        for c in "admin":
            q.key(c)
        time.sleep(0.3)
        q.key("ret")
        time.sleep(2.5)               # desktop settles

        off = 0
        # ---- phase A: idle -------------------------------------------
        time.sleep(6)
        txt, off = serial_tail(off)
        report("A idle", fps_lines(txt))

        # ---- phase B: sustained pointer motion ------------------------
        # Sweep back and forth; every move forces a cursor repaint.
        t_end = time.time() + 8
        dx = 24
        while time.time() < t_end:
            q.rel(dx, 0)
            if abs(dx) >= 1:
                pass
            # bounce
            dx = -dx if abs(dx) == 24 else dx
        txt, off = serial_tail(off)
        report("B pointer motion", fps_lines(txt))

        q.cmd("quit")
        s.close()
    finally:
        try:
            p.terminate()
        except Exception:
            pass
        time.sleep(1)
        try:
            p.kill()
        except Exception:
            pass


if __name__ == "__main__":
    main()
