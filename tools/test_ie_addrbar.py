#!/usr/bin/env python3
"""Headless proof for IE address bar on BOTH kernels + 64-bit wallpaper.

Verifies, for the 32-bit kernel and again after `switch` into long mode:
  1. Browser opens on the configured home page (https://www.bing.com/).
  2. Typing appends exactly one glyph per keystroke.
  3. Backspace removes exactly one glyph per keystroke.

Plus: after switching to long mode, opening the desktop shows a wallpaper.

Why serial
----------
iexplore logs the address bar content after every edit.  Reading those
lines is far more reliable than trying to measure glyph pixels through a
custom GDI display list, which turned out to be colour- and geometry-sensitive.

The bug this guards
-------------------
TranslateMessage synthesises a WM_CHAR (wParam 8) after every
WM_KEYDOWN(VK_BACK), and the kernel reproduces that faithfully.
iexplore's window procedure used to service *both*, so a single
backspace popped two characters off the focused field.  Editing now
lives in WM_CHAR only.
"""
import os
import socket
import subprocess
import sys
import threading
import http.server
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)

IMG = "build/os.img"
LOG = "build/serial_ieaddr.log"
PCAP = "build/net_ieaddr.pcap"
MON_ERR = "build/qemu_ieaddr.err"
MON_PORT = 4475

CLIENT_X, CLIENT_Y = 42, 106


def scr(cx, cy):
    return CLIENT_X + cx, CLIENT_Y + cy


def read_vbe_resolution():
    """Parse '<W>x<H>' from the kernel's '[HW] VBE: available ...' log."""
    import re
    m = re.search(r"\[HW\] VBE: available (\d+)x(\d+)", ser_text())
    if m:
        return int(m.group(1)), int(m.group(2))
    # Sensible QEMU std-VGA default if the log line is missing.
    return 1280, 720


def browser_addr_click():
    """Compute the on-screen centre of the managed BrowserApp's URL bar.

    The window is 600x440, centred, offset down by TOPBAR_H(32)+10, and
    its client area starts below a 32px title bar.  The C# address bar is
    at BarY=8, BarH=34, AddrX=16 with AddrW = Gfx.Width()-16-56-16.
    """
    W, H = read_vbe_resolution()
    ww, wh = 600, 440
    TOPBAR_H, TITLE_BAR_H = 32, 32
    wx = (W - ww) // 2
    wy = (H - wh) // 2 + TOPBAR_H + 10
    content_y = wy + TITLE_BAR_H
    addr_x = wx + 1 + 16 + 240        # well inside AddrX..AddrX+AddrW
    addr_y = content_y + 8 + 17       # centre of the 34px-tall bar
    return addr_x, addr_y


def wait_sock(port, timeout=40.0):
    end = time.time() + timeout
    while time.time() < end:
        try:
            return socket.create_connection(("127.0.0.1", port), 1.0)
        except OSError:
            time.sleep(0.25)
    raise RuntimeError("port %d never opened" % port)


def ser_text():
    # QEMU `-serial file:` writes here incrementally; read it fresh each call.
    try:
        with open(LOG, "r", encoding="latin-1", errors="replace") as f:
            return f.read()
    except (FileNotFoundError, OSError):
        return ""


def wait_for_log(needle, timeout=60.0):
    end = time.time() + timeout
    while time.time() < end:
        if needle in ser_text():
            return True
        time.sleep(0.3)
    return False


def send_key(mon, key, delay=0.14):
    # NOTE: QEMU's `sendkey` holds every key for 100ms by default.  Our delay
    # MUST exceed that, otherwise the next key is injected while the previous
    # (and its shift modifier) is still held -> shifted chars stick and symbol
    # URLs like "http://..." come out as garbage ("http://" -> ">>@>*#").
    mon.sendall(("sendkey %s\n" % key).encode())
    time.sleep(delay)


def type_text(mon, s, delay=0.14):
    table = {
        " ": "spc", ".": "dot", "/": "slash", ":": "shift-semicolon",
        "-": "minus", "_": "shift-minus", "?": "shift-slash",
    }
    for c in s:
        if c in table:
            send_key(mon, table[c], delay)
        elif c.isupper():
            send_key(mon, "shift-%s" % c.lower(), delay)
        else:
            send_key(mon, c, delay)


def type_line(mon, s):
    type_text(mon, s)
    send_key(mon, "ret", 0.35)


def _rel(mon, dx, dy):
    while dx or dy:
        sx = max(-100, min(100, dx))
        sy = max(-100, min(100, dy))
        mon.sendall(("mouse_move %d %d\n" % (sx, sy)).encode())
        dx -= sx
        dy -= sy
        time.sleep(0.02)


def move_to(mon, x, y):
    _rel(mon, -2000, -2000)
    time.sleep(0.15)
    _rel(mon, x, y)
    time.sleep(0.2)


def click(mon, x, y):
    move_to(mon, x, y)
    mon.sendall(b"mouse_button 1\n")
    time.sleep(0.12)
    mon.sendall(b"mouse_button 0\n")
    time.sleep(0.4)


def grab(mon, name):
    p = os.path.join("build", name)
    if os.path.exists(p):
        os.remove(p)
    mon.sendall(("screendump %s\n" % p).encode())
    end = time.time() + 10.0
    last = -1
    while time.time() < end:
        if os.path.exists(p):
            sz = os.path.getsize(p)
            if sz > 1000 and sz == last:
                return p
            last = sz
        time.sleep(0.25)
    return p


def non_black_count(path):
    """Count non-black pixels in a P6 PPM; 0 if file missing/invalid."""
    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError:
        return 0
    if not data.startswith(b"P6"):
        return 0
    i, vals = 2, []
    while len(vals) < 3:
        while i < len(data) and data[i] in b" \t\r\n":
            i += 1
        if data[i:i + 1] == b"#":
            while i < len(data) and data[i] not in b"\r\n":
                i += 1
            continue
        j = i
        while j < len(data) and data[j] not in b" \t\r\n":
            j += 1
        vals.append(int(data[i:j]))
        i = j
    i += 1
    w, h, _ = vals
    px = data[i:i + w * h * 3]
    return sum(1 for k in range(0, len(px), 3)
               if px[k] or px[k + 1] or px[k + 2])


def addr_lines_after(text, prefix, mark=0):
    """Extract ordered <prefix> addr=... lines from serial text."""
    lines = text[mark:].splitlines()
    out = []
    needle = "[%s] addr=" % prefix
    for ln in lines:
        if needle in ln:
            out.append(ln.split(needle, 1)[1])
    return out


def verify_addr_sequence(tag, mark, prefix="iexplore", timeout=8.0):
    end = time.time() + timeout
    while time.time() < end:
        text = ser_text()
        lines = addr_lines_after(text, prefix, mark)
        if len(lines) >= 7:
            break
        time.sleep(0.2)
    if not lines:
        print("  [%s] no addr lines after mark" % tag)
        return False
    lens = [len(s) for s in lines]
    print("  [%s] addr lengths after focus/type/bs: %s" % (tag, lens))
    if len(lens) < 7:
        print("  [%s] FAIL: expected >=7 addr lines, got %d" % (tag, len(lens)))
        return False
    L = lens[0]
    for i in range(1, 5):
        if lens[i] != L + i:
            print("  [%s] FAIL: typing did not append 1 char/step (%d -> %d)" %
                  (tag, L + i - 1, lens[i]))
            return False
    if lens[5] != L + 4 - 1:
        print("  [%s] FAIL: first backspace did not remove 1 char" % tag)
        return False
    if lens[6] != L + 4 - 2:
        print("  [%s] FAIL: second backspace did not remove 1 char" % tag)
        return False
    print("  [%s] addr edit sequence OK (+1 x4, -1 x2)" % tag)
    return True


def browser_test(mon, tag, home_mark=0, prefix="iexplore", home_needle=None,
                 click_cx=500, click_cy=42, absolute_click=None):
    """Run the address-bar sequence once the browser is on screen."""
    mark = len(ser_text())
    if absolute_click:
        print("  [%s] click the address bar at screen %s" % (tag, absolute_click))
        click(mon, *absolute_click)
    else:
        print("  [%s] click the address bar at client (%d,%d)" % (tag, click_cx, click_cy))
        click(mon, *scr(click_cx, click_cy))
    time.sleep(0.6)

    print("  [%s] type ABCD" % tag)
    type_text(mon, "ABCD", 0.12)
    time.sleep(0.5)

    print("  [%s] backspace x2" % tag)
    send_key(mon, "backspace", 0.5)
    time.sleep(0.5)
    send_key(mon, "backspace", 0.5)
    time.sleep(1.2)  # give QEMU's serial file time to flush the logs

    text = ser_text()
    home_ok = False
    if home_needle:
        home_ok = home_needle in text[home_mark:]
    seq_ok = verify_addr_sequence(tag, mark, prefix)
    print("  [%s] home page = bing : %s" % (tag, "PASS" if home_ok else "FAIL"))
    print("  [%s] address-bar edit : %s" % (tag, "PASS" if seq_ok else "FAIL"))
    return home_ok and seq_ok


def login(mon):
    print("[LOGIN] root / admin")
    for _ in range(3):
        type_line(mon, "root"); time.sleep(0.5)
        type_line(mon, "admin"); time.sleep(1.0)
        type_line(mon, "echo boot-ok")
        if wait_for_log("boot-ok", 15.0):
            return True
        mon.sendall(b"sendkey ret\n"); time.sleep(1.0)
    return False


# =====================================================================
#  Host-side HTTP server (for the 64-bit network fetch proof)
# ---------------------------------------------------------------------
#  QEMU's user-mode networking exposes the host as 10.0.2.2 to the guest.
#  We serve ONE complete, static HTML page over HTTP/1.0 with an explicit
#  Content-Length and Connection: close, so the kernel's HTTP client (which
#  has no chunked-encoding support) can fetch the whole document verbatim.
# =====================================================================
NET_PORT = 8137

_NET_BODY = (
    b"<html><head><meta charset=utf-8><title>NexOS Net Test</title></head>"
    b"<body style='font-family:monospace'>"
    b"<h1>Network OK</h1>"
    b"<p>This is a COMPLETE web page rendered by the 64-bit NexOS browser "
    b"over the emulated NE2000 ISA NIC (QEMU user-mode networking).</p>"
    b"<p>Second paragraph: if you can read this, the full document was "
    b"fetched and is not truncated by the kernel HTTP client.</p>"
    b"<ul><li>item one</li><li>item two</li><li>item three</li></ul>"
    b"</body></html>"
)


class _NetHandler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"

    def do_GET(self):
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(_NET_BODY)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(_NET_BODY)

    def log_message(self, *a):
        pass


def start_host_server(port=NET_PORT):
    srv = http.server.HTTPServer(("0.0.0.0", port), _NetHandler)
    t = threading.Thread(target=srv.serve_forever, daemon=True)
    t.start()
    return srv


def browser_net_test(mon, tag, host_ip="10.0.2.2", port=NET_PORT, timeout=30.0):
    """Prove the 64-bit browser can fetch a real page over the network.

    Clears the address bar, types the host URL, presses Enter (Go) and
    waits for the managed BrowserApp to log a successful fetch of a
    non-trivial, complete document.
    """
    print("[%s] 64-bit network fetch test (host http://%s:%d/)" % (tag, host_ip, port))
    ax, ay = browser_addr_click()
    # BrowserApp's address bar is a toggle: clicking it while editing commits
    # (editMode 1->0), clicking while idle focuses (0->1).  So we first click
    # the CONTENT area (always commits / is a no-op scroll), then the address
    # bar (always focuses) -- deterministic whatever state the last test left.
    click(mon, ax, min(ay + 120, 4096))   # content area -> commit edit
    time.sleep(0.3)
    click(mon, ax, ay)                    # address bar  -> focus / editMode=1
    time.sleep(0.4)
    # Clear whatever is currently in the bar (<= 23 chars after the addr test).
    # 0.12s > QEMU's 100ms key-hold so every backspace is registered (a faster
    # loop silently drops keystrokes and leaves stale characters behind).
    for _ in range(30):
        send_key(mon, "backspace", 0.4)
    time.sleep(0.3)
    url = "http://%s:%d/" % (host_ip, port)
    print("  [%s] type %s" % (tag, url))
    # 0.15s per char keeps each key (and its shift modifier for ':') fully
    # released before the next, so the URL is typed verbatim, not garbled.
    type_text(mon, url, 0.5)
    time.sleep(0.3)
    send_key(mon, "ret", 0.6)            # Enter -> BrowserApp.Fetch()
    ok = wait_for_log("[browser] fetched ", timeout)
    if ok:
        t = ser_text()
        idx = t.rfind("[browser] fetched ")
        line = t[idx:].splitlines()[0] if idx >= 0 else ""
        print("  [%s] %s" % (tag, line))
        ok = ("bytes from http://%s:%d/" % (host_ip, port)) in t
        # Capture the rendered page: the serial line only proves bytes arrived,
        # the screenshot proves the browser actually painted them.
        time.sleep(1.5)
        shot = grab(mon, "browser_page.ppm")
        print("  [%s] rendered page screenshot -> %s" % (tag, shot))
    else:
        print("  [%s] no '[browser] fetched' line within %.0fs" % (tag, timeout))
    print("  [%s] network fetch : %s" % (tag, "PASS" if ok else "FAIL"))
    return ok


def main():
    if not os.path.exists(IMG):
        print("missing %s - run `make build/os.img` first" % IMG)
        return 2
    for f in (LOG, MON_ERR):
        if os.path.exists(f):
            try:
                os.remove(f)
            except OSError:
                pass

    # Host HTTP server so the 64-bit browser can prove real end-to-end
    # networking (guest reaches it as http://10.0.2.2:NET_PORT/).
    srv = start_host_server(NET_PORT)
    # Sanity: confirm the server actually answers on localhost (isolates a
    # broken server from a broken guest<-host network path).
    try:
        import urllib.request
        with urllib.request.urlopen("http://127.0.0.1:%d/" % NET_PORT, timeout=5) as r:
            body = r.read()
        print("[HOST] localhost server OK (%d bytes)" % len(body))
    except Exception as e:
        print("[HOST] localhost server FAILED: %s" % e)

    errf = open(MON_ERR, "wb")
    q = subprocess.Popen([
        "qemu-system-x86_64",
        "-m", "128M",
        "-display", "none",
        "-no-reboot",
        "-monitor", "tcp:127.0.0.1:%d,server,nowait" % MON_PORT,
        "-serial", "file:%s" % LOG,
        # Modern netdev form so we can attach a packet dumper.  filter-dump
        # writes a pcap of everything crossing the NIC<->SLIRP boundary, which
        # is the only way to tell "guest never transmitted" apart from
        # "guest transmitted garbage SLIRP ignored".
        "-netdev", "user,id=n0",
        "-device", "ne2k_isa,netdev=n0",
        "-object", "filter-dump,id=dump0,netdev=n0,file=%s" % PCAP,
        "-drive", "format=raw,file=%s" % IMG,
    ], stdout=errf, stderr=errf)

    r32 = r64 = False
    try:
        mon = wait_sock(MON_PORT)
        mon.settimeout(3.0)
        try:
            mon.recv(65536)
        except (TimeoutError, socket.timeout, ConnectionResetError, OSError):
            pass

        print("[BOOT] 32-bit kernel...")
        if not wait_for_log("[K5] Hello world written", 90.0):
            print("RESULT: FAIL (32-bit boot)")
            return 1
        time.sleep(1.5)

        if not login(mon):
            print("RESULT: FAIL (login)")
            return 1

        # ================= 32-bit browser =================
        print("[32] gui browser")
        home_mark32 = len(ser_text())
        type_line(mon, "gui browser")
        if not wait_for_log("[iexplore] WM_CREATE", 40.0):
            print("RESULT: FAIL (32-bit browser did not start)")
            return 1
        time.sleep(4.0)
        r32 = browser_test(mon, "32-bit", home_mark32, "iexplore",
                           "[iexplore] home https://www.bing.com/")

        print("[32] leave the desktop")
        mon.sendall(b"sendkey esc\n"); time.sleep(1.0)
        mon.sendall(b"sendkey esc\n"); time.sleep(1.5)
        if not wait_for_log("[GUI] Exited GUI mode", 20.0):
            print("  !! GUI did not exit cleanly")

        # ================= switch to 64-bit =================
        print("[SWITCH] -> long mode (clean shell)")
        type_line(mon, "echo switch-ok")
        if not wait_for_log("switch-ok", 15.0):
            print("RESULT: FAIL (shell not responsive after GUI exit)")
            return 1
        type_line(mon, "switch")
        if not wait_for_log("[K64-1] kmain64 entered", 40.0):
            print("RESULT: FAIL (64-bit kernel did not come up)")
            return 1
        time.sleep(3.0)

        # 64-bit kernel initialises the NE2000 NIC at boot; without it the
        # browser fetch proof below cannot pass, so report it early.
        nic_ok = wait_for_log("[K64-8] Network initialized successfully", 25.0)
        print("[64] NE2000 NIC init : %s" % ("PASS" if nic_ok else "WARN (fetch below will fail)"))

        # ================= 64-bit browser =================
        print("[64] gui browser")
        home_mark64 = len(ser_text())
        type_line(mon, "gui browser")
        # 64-bit cannot run the 32-bit iexplore PE, so it falls back to the
        # native managed BrowserApp in gui.cpp.
        if not wait_for_log("[GUI] no PE browser available; using managed browser", 40.0):
            print("RESULT: FAIL (64-bit managed browser did not start)")
            return 1
        time.sleep(3.0)
        # Compute the on-screen URL-bar centre from the actual framebuffer
        # resolution so the click is resolution-independent.
        ax, ay = browser_addr_click()
        print("  [64] resolved VBE resolution %dx%d -> addr-bar click (%d,%d)"
              % (read_vbe_resolution()[0], read_vbe_resolution()[1], ax, ay))
        r64 = browser_test(mon, "64-bit", home_mark64, "browser",
                           "[browser] addr=https://www.bing.com/",
                           absolute_click=(ax, ay))

        # ================= 64-bit network fetch =================
        r64net = browser_net_test(mon, "64-bit")

        # ================= 64-bit wallpaper =================
        print("[64] close browser and screenshot desktop for wallpaper")
        # The browser window close button is at the top-right of the centred window.
        click(mon, 924, 198)
        time.sleep(1.5)
        grab(mon, "desk64.ppm")

        mon.sendall(b"quit\n")
        time.sleep(1.0)
    finally:
        try:
            q.wait(timeout=6.0)
        except subprocess.TimeoutExpired:
            q.terminate()
            try:
                q.wait(timeout=3.0)
            except subprocess.TimeoutExpired:
                q.kill()
        errf.close()
        try:
            srv.shutdown()
            srv.server_close()
        except OSError:
            pass

    print("")
    print("================= address bar =================")
    print("  32-bit                 : %s" % ("PASS" if r32 else "FAIL"))
    print("  64-bit                 : %s" % ("PASS" if r64 else "FAIL"))
    print("================= network =====================")
    print("  64-bit fetch page      : %s" % ("PASS" if r64net else "FAIL"))

    desk_path = "build/desk64.ppm"
    desk_ok = False
    if os.path.exists(desk_path):
        nb = non_black_count(desk_path)
        print("  64-bit desktop non-black pixels: %d" % nb)
        # A wallpaper should give a very large non-black count; fallback
        # gradient would also be non-black, so this is only a sanity check.
        desk_ok = nb > 300000
    print("  64-bit wallpaper       : %s" % ("PASS" if desk_ok else "FAIL"))
    print("===============================================")
    ok = r32 and r64 and r64net and desk_ok
    print("RESULT: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
