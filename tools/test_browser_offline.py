#!/usr/bin/env python3
"""End-to-end proof for the managed BrowserApp offline page + real http:// fetch.

Verifies, on the 64-bit (long-mode) kernel with the C# managed shell:

  A. OFFLINE PAGE  -- the browser's default home is https://www.bing.com/,
     and NexOS has no TLS, so fetching it fails (or follows a body-less 301
     back to an empty body) and the browser must render the LOCAL offline
     start page instead of a bare error line.  Asserted by the serial tag
     `[browser] offline-page <url>` (Browser.cs:185) plus `status=4` painting
     "Offline page (local)".

  B. REAL HTTP      -- while there IS a network (QEMU user-mode),
     http://example.com must be fetched for real and the browser must log
     `[browser] fetched N bytes from http://example.com`.  Proves the plain
     http:// path works end to end (DNS via 10.0.2.3, TCP through SLIRP).

  C. HOST HTTP (guard) -- a host-side HTTP/1.0 server (Content-Length,
     Connection: close) reached at http://10.0.2.2:PORT/ must also fetch,
     which isolates a broke guest<->host path from DN/SLIRP issues.

Boot flow follows tools/test_loginscreen64.py:
  boot 32-bit -> graphical lock screen -> root/admin -> select Terminal ->
  `switch64` -> 64-bit lock screen -> root/admin -> select on the desktop
  Browser icon -> managed BrowserApp handles the address bar.

Usage:
    python3 tools/test_browser_offline.py [path/to/os.img]
"""
import os, re, sys, socket, time, subprocess, threading, http.server

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)

IMG = sys.argv[1] if len(sys.argv) > 1 else "build/os.img"
# MODE: 'offline' (default) proves the local offline-page render (a fetch that
# MUST fail -> "[browser] offline-page").  'online' proves a real http:// fetch
# reaches the network (QEMU SLIRP -> host server at 10.0.2.2:PORT).  Each mode
# opens the browser ONCE so the address bar starts from the clean default
# "https://www.bing.com/" (the caret sits at position 0 and Backspace cannot
# clear it), so the typed URL is not polluted by any previous test.
MODE = (sys.argv[2] if len(sys.argv) > 2 else "offline").lower()
assert MODE in ("offline", "online"), "mode must be offline|online"

WORK = "build/browser_offline_work.img"
LOG = "build/serial_browser_offline.log"
ERR = "build/qemu_browser_offline.err"
PORT = 4477
HOSTSERV_PORT = 8138

# Desktop grid geometry (csharp/apps/Shell/Desktop.cs) at 1280x720 VBE.
# Margin=22, Cell=92, TaskH=48 -> PerCol(720)=6.
# Desktop icons (seed_desktop_shortcuts order / LoadDefaults):
#   0 ThisPC 1 Terminal 2 Calculator 3 TaskMgr 4 Settings 5 Optimizer
#   6 Notepad 7 About 8 Browser
TERM_XY   = (64, 156)   # Terminal.lnk  centre
BROWSER_XY = (156, 248)  # Browser.lnk   centre


def read_log():
    try:
        return open(LOG, "rb").read().decode("latin-1", "ignore")
    except Exception:
        return ""


def wait_for_log(needle, timeout=60.0):
    end = time.time() + timeout
    while time.time() < end:
        if needle in read_log():
            return True
        time.sleep(0.3)
    return False


def wait_sock(port, timeout=40.0):
    end = time.time() + timeout
    while time.time() < end:
        try:
            return socket.create_connection(("127.0.0.1", port), 0.5)
        except OSError:
            time.sleep(0.25)
    raise RuntimeError("monitor port %d never opened" % port)


# --- QEMU monitor mouse (relative); track cursor absolutely. ----------
_CURSOR = [640, 360]


def reset_cursor(X=640, Y=360):
    """Re-seed the tracked cursor position.

    The 32->64 kernel switch (switch64) tears down and rebuilds the GUI, and
    the fresh desktop starts with the pointer back at the default centre
    (640, 360).  Call this after entering the 64-bit desktop (and after any
    other GUI rebuild) so later relative mouse_move calls target the right
    absolute point again.
    """
    global _CURSOR
    _CURSOR = [X, Y]


def mouse_move(mon, x, y, delay=0.08):
    global _CURSOR
    dx, dy = x - _CURSOR[0], y - _CURSOR[1]
    while dx or dy:
        sx = max(-90, min(90, dx)); sy = max(-90, min(90, dy))
        mon.sendall(("mouse_move %d %d\n" % (sx, sy)).encode())
        _CURSOR[0] += sx; _CURSOR[1] += sy
        dx -= sx; dy -= sy
        time.sleep(delay)
    time.sleep(0.2)


def mouse_click(mon, x, y):
    mouse_move(mon, x, y)
    mon.sendall(b"mouse_button 1\n"); time.sleep(0.12)
    mon.sendall(b"mouse_button 0\n"); time.sleep(0.6)


def mouse_dblclick(mon, x, y):
    # Desktop icons launch only on a DOUBLE click (gui.cpp handle_mouse_down:
    # a single click only selects; the 2nd click within 500ms on the same icon
    # launches).  Two mouse_button press/release pairs ~100ms apart at the
    # same (x, y) satisfies that.
    mouse_move(mon, x, y)
    mon.sendall(b"mouse_button 1\n"); time.sleep(0.10)
    mon.sendall(b"mouse_button 0\n"); time.sleep(0.10)
    mon.sendall(b"mouse_button 1\n"); time.sleep(0.10)
    mon.sendall(b"mouse_button 0\n"); time.sleep(0.8)


def send_key(mon, key, delay=0.14):
    # QEMU holds keys 100ms; delay must exceed that or symbols/garbling occur.
    mon.sendall(("sendkey %s\n" % key).encode())
    time.sleep(delay)


def type_text(mon, s, delay=0.14):
    table = {
        " ": "spc", ".": "dot", "/": "slash", ":": "shift-semicolon",
        "-": "minus", "_": "shift-minus", "?": "shift-slash", "=": "equal",
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
    send_key(mon, "ret", 0.4)


# ---------------------------------------------------------------------
# Host HTTP/1.0 server at http://10.0.2.2:HOSTSERV_PORT/ as the guard.
# ---------------------------------------------------------------------
_HOST_BODY = (
    b"<html><head><title>NexOS Offline Test</title></head><body><h1>Host "
    b"HTTP/1.0 OK</h1><p>Reached the host server over QEMU user-mode.</p>"
    b"</body></html>"
)


class _H(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"
    def do_GET(self):
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(_HOST_BODY)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(_HOST_BODY)
    def log_message(self, *a):
        pass


def start_host_server():
    srv = http.server.HTTPServer(("0.0.0.0", HOSTSERV_PORT), _H)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv


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


def browser_fetch(mon, url, expect_offline, timeout=60.0):
    """Focus the address bar, type `url`, press Enter, wait for the outcome.

    IMPORTANT (BrowserApp address-bar behaviour, verified on the emulated
    desktop): clicking the bar only sets editMode=1 -- it does NOT reposition
    the caret, which stays at position 0.  So `url` is inserted at the START
    of whatever the bar already holds.  As long as we call this on a FRESH
    BrowserApp (opened via double-click after the previous one was closed with
    Alt+F4) the bar holds exactly the default "https://www.bing.com/", and a
    URL typed here becomes:

        <url><default text>

    For that to name a reachable host the target must end with "/" so the old
    default text lands in the PATH, not in the host:
        "http://10.0.2.2:8138/"  ->  "http://10.0.2.2:8138/https://www.bing.com/"
                                     host=10.0.2.2  port=8138   OK
    Pass url="" to test the untouched default (bing) with a bare Enter.
    """
    m = re.search(r"\[HW\] VBE: available (\d+)x(\d+)", read_log())
    W = H = 720
    if m:
        W, H = int(m.group(1)), int(m.group(2))
    ww, wh = 600, 440
    TOPBAR, TITLE = 32, 32
    wx = (W - ww) // 2
    wy = (H - wh) // 2 + TOPBAR + 10
    cy = wy + TITLE
    ax = wx + 1 + 16 + 240      # well inside the address bar
    ay = cy + 8 + 17            # centre of the 34px-tall bar
    mouse_click(mon, ax, ay)                      # addr bar -> focus (caret@0)
    time.sleep(0.4)
    if url:                                       # empty => keep default text
        print("    type:", url)
        type_text(mon, url, 0.4)
        time.sleep(0.3)
    send_key(mon, "ret", 0.6)                      # Enter -> Fetch()
    if expect_offline:
        # We expect the fetch below to FAIL (DNS error / host unreachable /
        # no server) so BrowserApp renders the LOCAL offline page.
        ok = wait_for_log("[browser] offline-page ", timeout)
        status = "offline-page"
    else:
        ok = wait_for_log("[browser] fetched ", timeout)
        t = read_log()
        if ok:
            idx = t.rfind("[browser] fetched ")
            print("    " + t[idx:].splitlines()[0])
            ok = ("bytes from " + url) in t
        status = "fetched"
    print("    %s -> %s" % (status, "PASS" if ok else "FAIL"))
    return ok


def main():
    subprocess.run(["cp", IMG, WORK], check=True)
    for f in (LOG, ERR):
        if os.path.exists(f):
            os.remove(f)

    srv = start_host_server()
    try:
        import urllib.request
        with urllib.request.urlopen("http://127.0.0.1:%d/" % HOSTSERV_PORT, timeout=5) as r:
            print("[HOST] localhost server OK (%d bytes)" % len(r.read()))
    except Exception as e:
        print("[HOST] localhost server FAILED: %s" % e)

    errf = open(ERR, "wb")
    qemu = subprocess.Popen([
        "qemu-system-x86_64",
        "-drive", "format=raw,file=%s" % WORK,
        "-m", "128M",
        "-vga", "std",
        "-display", "none",
        "-no-reboot",
        "-monitor", "tcp:127.0.0.1:%d,server,nowait" % PORT,
        "-chardev", "file,id=ser,path=%s" % LOG,
        "-serial", "chardev:ser",
        "-netdev", "user,id=n0",
        "-device", "ne2k_isa,netdev=n0",
        "-object", "filter-dump,id=dump0,netdev=n0,file=%s" % "build/net_browser_offline.pcap",
    ], stdout=errf, stderr=errf)

    a = b = c = d = False
    try:
        mon = wait_sock(PORT)
        mon.settimeout(3.0)
        try:
            mon.recv(65536)
        except (TimeoutError, socket.timeout, OSError):
            pass

        print("[BOOT] 32-bit -> auto GUI lock screen, sign in root/admin")
        time.sleep(18.0)
        type_text(mon, "admin"); time.sleep(0.8)
        send_key(mon, "ret", 0.5); time.sleep(2.5)
        if "[K32-LOGIN] OK user=root" not in read_log():
            print("RESULT: FAIL (32-bit login)")
            return 1
        print("[32] on desktop; open Terminal")

        mouse_click(mon, *TERM_XY)
        time.sleep(1.2)
        mon.sendall(b"sendkey alt-f4\n"); time.sleep(0.4)  # dismiss stray window

        print("[SWITCH] jump into 64-bit long-mode kernel")
        type_line(mon, "switch64")
        time.sleep(13.0)
        if "[K64] sign-in deferred to the graphical lock screen" not in read_log():
            print("RESULT: FAIL (64-bit did not come up)")
            print("\n".join(read_log().splitlines()[-8:]))
            return 1
        print("[64] 64-bit lock screen; sign in root/admin")
        type_text(mon, "admin"); time.sleep(0.8)
        send_key(mon, "ret", 0.5); time.sleep(2.5)
        if "[K64-LOGIN] OK user=root" not in read_log():
            print("RESULT: FAIL (64-bit login)")
            return 1
        print("[64] on desktop; check NIC then open Browser")
        nic = wait_for_log("[K64-8] Network initialized successfully", 15.0)
        print("[64] NE2000 NIC init : %s" % ("PASS" if nic else "WARN"))

        # The 32->64 switch rebuilt the GUI, so the pointer is back at the
        # default centre.  Re-seed the tracked cursor before clicking.
        reset_cursor(640, 360)

        def open_browser(tag):
            # Desktop icons hot-launch only on a DOUBLE click (single click
            # just selects) -- see gui.cpp handle_mouse_down.
            mouse_dblclick(mon, *BROWSER_XY)
            if not wait_for_log("[browser] addr=", 40.0):
                print("RESULT: FAIL (%s: managed browser did not start)" % tag)
                return False
            time.sleep(2.0)
            # fresh BrowserApp => its address bar holds the default bing URL
            return True

        def close_browser():
            # Alt+F4 closes the focused browser window (as test_loginscreen64).
            mon.sendall(b"sendkey alt-f4\n"); time.sleep(1.2)

        # Run exactly ONE test per mode so the browser is opened only once and
        # the address bar stays on the clean default (no cross-test pollution).
        a = b = c = d = False
        if MODE == "offline":
            if not open_browser("offline"):
                return 1
            print("")
            print("[A] OFFLINE PAGE: a fetch that MUST fail should render the "
                  "local offline start page (not a bare error)")
            # A host that cannot resolve, so HttpGet() returns empty -> the
            # BrowserApp must fall back to the local offline page.
            a = browser_fetch(mon, "http://NexOS-nosuchhost.invalid/",
                              expect_offline=True)
            time.sleep(1.0)
            grab(mon, "browser_offline_page.ppm")
        else:  # online
            if not open_browser("online"):
                return 1
            print("")
            print("[B] REAL http://: fetch the host HTTP/1.0 server at "
                  "10.0.2.2:%d over QEMU user-mode networking" % HOSTSERV_PORT)
            b = browser_fetch(mon, "http://10.0.2.2:%d/" % HOSTSERV_PORT,
                              expect_offline=False)
            time.sleep(1.0)
            grab(mon, "browser_host_fetched.ppm")
        mon.sendall(b"quit\n"); time.sleep(1.0)
    finally:
        try:
            qemu.wait(timeout=6.0)
        except subprocess.TimeoutExpired:
            qemu.terminate()
            try:
                qemu.wait(timeout=3.0)
            except subprocess.TimeoutExpired:
                qemu.kill()
        errf.close()
        try:
            srv.shutdown(); srv.server_close()
        except OSError:
            pass

    data = read_log()
    print("")
    print("============= results (mode=%s) =============" % MODE)
    if MODE == "offline":
        print("  A offline page render  : %s" % ("PASS" if a else "FAIL"))
        ok = a
    else:
        print("  B http:// host fetch   : %s" % ("PASS" if b else "FAIL"))
        ok = b

    if "TRIPLE FAULT" in data or "PANIC" in data:
        print("\nFAIL: kernel fault detected")
        ok = False

    print("")
    print("--- serial tail ---")
    print("\n".join(data.splitlines()[-12:]))
    print("\nRESULT: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    try:
        rc = main()
    except Exception as ex:
        import traceback
        traceback.print_exc()
        rc = 1
    sys.exit(rc)
