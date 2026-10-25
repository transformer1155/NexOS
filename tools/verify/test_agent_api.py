#!/usr/bin/env python3
"""GUI verification for the NexOS agent remote-API + local-API feature.

Boots the 64-bit kernel (os.img) with the NE2000 NIC and a host-forwarded
HTTP port, logs in, starts networking, then verifies everything through the
*served web GUI* rather than the serial CLI:

  1. GET  /agent            -> the graphical agent console page is served
  2. POST /api/agent        -> local agent returns JSON {"output": ...}
  3. POST /api/agent remote -> a local mock OpenAI-compatible server
                               (running on the host) is reached and its reply
                               is forwarded back as {"output": ...}

The two API calls are exactly what the /agent web page does when a user clicks
"Run agent", so passing this test means the GUI works end-to-end.
"""
import os, sys, socket, time, subprocess, threading, urllib.request, urllib.parse, json

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)

IMG = sys.argv[1] if len(sys.argv) > 1 else "build/os.img"
WORK = "build/agentapi.img"
LOG = "build/serial_agentapi.log"
MONPORT = 4458
HTTPPORT = 18080          # host-side forwarded port -> guest :8080
MOCKPORT = 18999          # host-side mock OpenAI-compatible server

QEMU = r"D:\qemu\qemu-system-x86_64.exe"
if not os.path.exists(QEMU):
    QEMU = "qemu-system-x86_64"


# ---- local mock OpenAI-compatible server ------------------------------------
class MockHandler(urllib.request.BaseHandler):
    pass

def mock_app(environ, start_response):
    # Echo a deterministic marker so we can prove the remote path fired.
    body = ""
    try:
        length = int(environ.get("CONTENT_LENGTH", "0") or "0")
        body = environ["wsgi.input"].read(length).decode("utf-8", "ignore")
    except Exception:
        pass
    # crude extraction of the user prompt (not needed, but nice for logs)
    reply = "MOCK_REPLY: remote agent reached OK"
    payload = json.dumps({"choices": [{"message": {"role": "assistant",
                                                    "content": reply}}]}).encode()
    start_response("200 OK", [("Content-Type", "application/json"),
                              ("Access-Control-Allow-Origin", "*")])
    return [payload]

# Use a tiny raw HTTP server (no wsgiref dependency surprises under Git Bash).
from http.server import BaseHTTPRequestHandler, HTTPServer

class _H(BaseHTTPRequestHandler):
    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0") or "0")
        body = self.rfile.read(n).decode("utf-8", "ignore")
        # Capture the Authorization header + requested model for the DeepSeek test.
        g_last_auth = self.headers.get("Authorization", "")
        g_last_body = body
        global LAST_AUTH, LAST_BODY
        LAST_AUTH, LAST_BODY = g_last_auth, body
        reply = "MOCK_REPLY: remote agent reached OK"
        payload = json.dumps({"choices": [{"message": {"role": "assistant",
                                                        "content": reply}}]}).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)
    def log_message(self, *a):
        pass

LAST_AUTH = ""
LAST_BODY = ""

def start_mock():
    srv = HTTPServer(("127.0.0.1", MOCKPORT), _H)
    t = threading.Thread(target=srv.serve_forever, daemon=True)
    t.start()
    return srv


def wait_sock(port, timeout=30.0):
    end = time.time() + timeout
    while time.time() < end:
        try:
            return socket.create_connection(("127.0.0.1", port), timeout=0.5)
        except OSError:
            time.sleep(0.2)
    raise RuntimeError("monitor not ready")


def type_line(mon, s):
    keymap = {' ': 'spc', '.': 'dot', '/': 'slash', '\\': 'backslash',
              '-': 'minus', '_': 'shift-minus'}
    for ch in s:
        key = "shift-%s" % ch.lower() if 'A' <= ch <= 'Z' else keymap.get(ch, ch)
        mon.sendall(("sendkey %s\n" % key).encode())
        time.sleep(0.07)
    mon.sendall(b"sendkey ret\n")
    time.sleep(0.4)


def http_get(path, timeout=5.0):
    return urllib.request.urlopen("http://127.0.0.1:%d%s" % (HTTPPORT, path),
                                  timeout=timeout).read()


def http_post(path, data, timeout=8.0):
    req = urllib.request.Request(
        "http://127.0.0.1:%d%s" % (HTTPPORT, path),
        data=urllib.parse.urlencode(data).encode(),
        headers={"Content-Type": "application/x-www-form-urlencoded"})
    return urllib.request.urlopen(req, timeout=timeout).read().decode("utf-8", "ignore")


def main():
    if not os.path.exists(IMG):
        print("ERROR: %s not found. Build it first." % IMG)
        return 1
    subprocess.run(["cp", IMG, WORK], check=True)
    for f in (LOG, "build/qemu_agentapi.err"):
        if os.path.exists(f):
            os.remove(f)

    mock = start_mock()
    print("[setup] mock OpenAI server on 127.0.0.1:%d" % MOCKPORT)

    errf = open("build/qemu_agentapi.err", "wb")
    qemu = subprocess.Popen([
        QEMU, "-machine", "pc",
        "-drive", "format=raw,file=%s" % WORK,
        "-m", "256M", "-accel", "tcg", "-vga", "std", "-display", "none",
        "-no-reboot",
        "-monitor", "tcp:127.0.0.1:%d,server,nowait" % MONPORT,
        "-net", "nic,model=ne2k_isa",
        "-net", "user,hostfwd=tcp::%d-:8080" % HTTPPORT,
        "-chardev", "file,id=ser,path=%s" % LOG,
        "-serial", "chardev:ser",
    ], stdout=errf, stderr=errf)

    fails = []
    try:
        mon = wait_sock(MONPORT)
        mon.settimeout(3.0)
        try:
            mon.recv(65536)
        except (TimeoutError, socket.timeout):
            pass
        time.sleep(9.0)
        # Log in and bring up networking (this is setup, not the verification).
        type_line(mon, "root")
        type_line(mon, "admin")
        time.sleep(1.0)
        print("[setup] starting network")
        type_line(mon, "netstart")
        time.sleep(2.0)

        # Wait for the HTTP server to accept connections.
        up = False
        for _ in range(40):
            try:
                http_get("/agent")
                up = True
                break
            except Exception:
                time.sleep(0.3)
        if not up:
            fails.append("HTTP server did not come up after netstart")
            print("FAIL: HTTP server unreachable")
        else:
            # ---- (1) graphical agent console page ----
            try:
                html = http_get("/agent").decode("utf-8", "ignore")
                if "NexOS Agent" not in html or "Run agent" not in html:
                    fails.append("/agent page missing GUI elements")
                    print("FAIL: /agent page malformed")
                else:
                    print("[GUI] /agent served OK (%d bytes, form + Run button present)" % len(html))
            except Exception as e:
                fails.append("/agent unreachable: %s" % e)
                print("FAIL: /agent unreachable")

            # ---- (2) local agent via /api/agent ----
            try:
                j = http_post("/api/agent", {"goal": "What is 2+2?"})
                print("[api] local /api/agent -> %s" % j[:120])
                if '"output"' not in j:
                    fails.append("local /api/agent returned no output field")
                    print("FAIL: local /api/agent malformed")
                elif len(j) < 20:
                    fails.append("local /api/agent output too short")
                    print("FAIL: local /api/agent empty")
                else:
                    print("[api] local agent responded OK")
            except Exception as e:
                fails.append("local /api/agent error: %s" % e)
                print("FAIL: local /api/agent error")

            # ---- (3) remote agent via /api/agent remote=1 ----
            try:
                remote_url = "http://10.0.2.2:%d/v1/chat/completions" % MOCKPORT
                j = http_post("/api/agent",
                              {"goal": "ping", "remote": "1", "url": remote_url})
                print("[api] remote /api/agent -> %s" % j[:160])
                if "MOCK_REPLY" not in j:
                    fails.append("remote /api/agent did not reach mock server")
                    print("FAIL: remote path did not return MOCK_REPLY")
                else:
                    print("[api] REMOTE agent reached mock OpenAI API OK")
            except Exception as e:
                fails.append("remote /api/agent error: %s" % e)
                print("FAIL: remote /api/agent error")

            # ---- (4) DeepSeek API key + model are forwarded to the remote ----
            try:
                remote_url = "http://10.0.2.2:%d/v1/chat/completions" % MOCKPORT
                j = http_post("/api/agent",
                              {"goal": "ping", "remote": "1", "url": remote_url,
                               "key": "sk-deepseek-test123", "model": "deepseek-chat"})
                if "MOCK_REPLY" not in j:
                    fails.append("deepseek-key path did not reach mock server")
                    print("FAIL: deepseek-key path returned no MOCK_REPLY")
                elif "Bearer sk-deepseek-test123" not in LAST_AUTH:
                    fails.append("Authorization: Bearer header not sent to remote")
                    print("FAIL: remote did not receive 'Authorization: Bearer sk-deepseek-test123' (got: %r)" % LAST_AUTH)
                elif '"model":"deepseek-chat"' not in LAST_BODY.replace(" ", ""):
                    fails.append("model=deepseek-chat not forwarded in request body")
                    print("FAIL: remote body missing model=deepseek-chat (got: %r)" % LAST_BODY[:200])
                else:
                    print("[api] DEEPSEEK key+model forwarded OK (auth=%r)" % LAST_AUTH)
            except Exception as e:
                fails.append("deepseek-key path error: %s" % e)
                print("FAIL: deepseek-key path error")

        mon.sendall(b"quit\n")
        time.sleep(1.0)
    finally:
        try:
            qemu.wait(timeout=5.0)
        except subprocess.TimeoutExpired:
            qemu.terminate()
            qemu.wait(timeout=3.0)
        mock.shutdown()
        errf.close()

    with open(LOG, "rb") as f:
        data = f.read().decode("latin-1", "ignore")
    if "EXCEPTION" in data:
        fails.append("kernel EXCEPTION detected")
        print("\nFAIL: kernel exception in serial log")

    print("\n--- serial log tail ---")
    print("\n".join(data.splitlines()[-20:]))
    print("\nGraphical agent console is available at:  http://localhost:%d/agent" % HTTPPORT)

    if fails:
        print("\nRESULT: FAIL")
        for f in fails:
            print("  - " + f)
        return 1
    print("\nRESULT: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
