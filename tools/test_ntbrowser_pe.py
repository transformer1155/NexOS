#!/usr/bin/env python3
"""Headless proof that the new native PE browser (winpe/ntbrowser.exe) does a
REAL http:// fetch on the 32-bit NexOS kernel, over QEMU user-mode networking
to a host-side HTTP/1.0 server.

ntbrowser.exe is a PE32 i386 guest loaded by win32_run (the wine/win32 PE
path).  It reaches the kernel's net_http_get() through the new NexOS.dll
bridge MiniHttpGet, bound at run time via GetProcAddress.  We launch it with a
start URL argument:

    winapp ntbrowser.exe http://10.0.2.2:PORT/

and assert the serial diagnostics it emits:
    [ntbrowser] window created
    [ntbrowser] MiniHttpGet bridge up
    [ntbrowser] fetched N bytes (http)      <- proves the real fetch happened

The Host.Get bridge reached through OutputDebugStringA -> "[app] <msg>".

Usage:
    python3 tools/test_ntbrowser_pe.py [path/to/os.img]
"""
import os, sys, socket, time, subprocess, threading, http.server

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)
IMG = sys.argv[1] if len(sys.argv) > 1 else "build/os.img"
WORK = "build/ntbr_work.img"
LOG = "build/serial_ntbr.log"
PORT = 4490
HOSTSERV_PORT = 8137

# Desktop: Terminal icon at col0 row1 on the C# managed grid (as in the
# loginscreen tests) -> click here to open the GUI Terminal window.
TERM_XY = (64, 156)
_CURSOR = [640, 360]

# Host HTTP/1.0 server body sent in response to the browser's GET.
_HOST_BODY = (
    b"NT Browser real-fetch test\n"
    b"This is the body that the PE browser fetched over the network.\n"
    b"<script>var n = 6 * 7; if (n == 42) { n = n + 1; } n</script>\n"
)


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
    raise RuntimeError("monitor port %d not ready" % port)


def key(mon, k, d=0.14):
    mon.sendall(("sendkey %s\n" % k).encode()); time.sleep(d)


def type_text(mon, s, d=0.14):
    tbl = {" ": "spc", "/": "slash", "\\": "backslash",
           ":": "shift-semicolon", ".": "dot", "-": "minus", "_": "shift-minus"}
    for c in s:
        if c in tbl: key(mon, tbl[c], d)
        elif c.isupper(): key(mon, "shift-%s" % c.lower(), d)
        else: key(mon, c, d)


def mouse_move(mon, x, y):
    global _CURSOR
    dx, dy = x - _CURSOR[0], y - _CURSOR[1]
    while dx or dy:
        sx = max(-90, min(90, dx)); sy = max(-90, min(90, dy))
        mon.sendall(("mouse_move %d %d\n" % (sx, sy)).encode())
        _CURSOR[0] += sx; _CURSOR[1] += sy
        dx -= sx; dy -= sy
        time.sleep(0.08)
    time.sleep(0.3)


def mouse_click(mon, x, y):
    mouse_move(mon, x, y)
    mon.sendall(b"mouse_button 1\n"); time.sleep(0.12)
    mon.sendall(b"mouse_button 0\n"); time.sleep(0.6)


class _H(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"
    def do_GET(self):
        self.send_response(200)
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.send_header("Content-Length", str(len(_HOST_BODY)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(_HOST_BODY)
    def log_message(self, *a):
        pass


def start_server():
    srv = http.server.HTTPServer(("0.0.0.0", HOSTSERV_PORT), _H)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv


def main():
    subprocess.run(["cp", IMG, WORK], check=True)
    for f in (LOG,):
        if os.path.exists(f): os.remove(f)
    srv = start_server()
    try:
        import urllib.request
        with urllib.request.urlopen("http://127.0.0.1:%d/" % HOSTSERV_PORT, timeout=5) as r:
            print("[HOST] localhost server OK (%d bytes)" % len(r.read()))
    except Exception as e:
        print("[HOST] localhost server FAILED: %s" % e)

    errf = open("build/qemu_ntbr.err", "wb")
    qemu = subprocess.Popen([
        "qemu-system-x86_64", "-drive", "format=raw,file=%s" % WORK,
        "-m", "128M", "-vga", "std", "-display", "none", "-no-reboot",
        "-monitor", "tcp:127.0.0.1:%d,server,nowait" % PORT,
        "-chardev", "file,id=ser,path=%s" % LOG, "-serial", "chardev:ser",
        "-netdev", "user,id=n0", "-device", "ne2k_isa,netdev=n0",
    ], stdout=errf, stderr=errf)
    ok = False
    try:
        mon = wait_sock(PORT); mon.settimeout(3.0)
        try: mon.recv(65536)
        except (TimeoutError, socket.timeout, OSError): pass

        print("[BOOT] 32-bit auto-GUI lock -> sign in root/admin")
        time.sleep(18.0)
        type_text(mon, "admin"); key(mon, "ret", 0.5); time.sleep(2.5)
        if "[K32-LOGIN] OK user=root" not in read_log():
            print("RESULT: FAIL (32-bit login)")
            return 1
        print("[32] on desktop; open GUI Terminal")
        _CURSOR[0], _CURSOR[1] = 640, 360
        mouse_click(mon, *TERM_XY)
        time.sleep(1.2)
        mon.sendall(b"sendkey alt-f4\n"); time.sleep(0.4)  # dismiss stray window

        print("[RUN] winapp ntbrowser.exe http://10.0.2.2:%d/" % HOSTSERV_PORT)
        type_text(mon, "winapp ntbrowser.exe http://10.0.2.2:%d/" % HOSTSERV_PORT,
                  0.16)
        key(mon, "ret", 0.6)
        time.sleep(2.0)

        bridged = wait_for_log("[ntbrowser] MiniHttpGet bridge up", 30.0)
        fetched = wait_for_log("[ntbrowser] fetched ", 60.0)
        ranjs = wait_for_log("[ntbrowser] js-selftest -> ", 20.0)
        created = "[ntbrowser] window created" in read_log()
        print("  window created   : %s" % ("PASS" if created else "FAIL"))
        print("  MiniHttpGet up   : %s" % ("PASS" if bridged else "FAIL"))
        print("  real http fetch  : %s" % ("PASS" if fetched else "FAIL"))
        print("  mini-JS ran      : %s" % ("PASS" if ranjs else "FAIL"))

        # print the fetched/selftest lines for the record
        for l in read_log().splitlines():
            if any(k in l for k in ("ntbrowser] fetched", "ntbrowser] fetch FAILED",
                                    "ntbrowser] js-selftest", "ntbrowser] script ran")):
                print("   ", l)

        ok = created and bridged and fetched and ranjs
        mon.sendall(b"quit\n"); time.sleep(1.0)
    finally:
        try: qemu.wait(timeout=6.0)
        except subprocess.TimeoutExpired:
            qemu.terminate()
            try: qemu.wait(timeout=3.0)
            except subprocess.TimeoutExpired: qemu.kill()
        errf.close()
        try: srv.shutdown(); srv.server_close()
        except OSError: pass

    data = read_log()
    print("\n--- serial tail ---")
    print("\n".join(l for l in data.splitlines() if l.strip())[-1500:])
    if "TRIPLE FAULT" in data or "PANIC" in data:
        print("\nFAIL: kernel fault detected")
        ok = False
    print("\nRESULT: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    try: rc = main()
    except Exception as ex:
        import traceback; traceback.print_exc(); rc = 1
    sys.exit(rc)
