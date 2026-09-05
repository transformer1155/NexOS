#!/usr/bin/env python3
"""Host-side TLS bridge for the NexOS in-OS agent -> DeepSeek (or any
OpenAI-compatible HTTPS endpoint).

WHY THIS EXISTS
---------------
The NexOS TCP stack only speaks plain HTTP (no TLS).  DeepSeek's API is
HTTPS-only and requires an `Authorization: Bearer <key>` header.  This
proxy runs on the *host* (the machine running QEMU / the real PC), listens
on plain HTTP, and terminates TLS to the upstream provider.  The guest OS
just points its remote endpoint at:

    http://<host-reachable-address>:<listen-port>/v1/chat/completions

and (optionally) sends its own API key as `Authorization: Bearer`.  The
proxy forwards the request over HTTPS to DeepSeek and relays the reply back.

USAGE
-----
    # DeepSeek (real key from https://platform.deepseek.com)
    DEEPSEEK_API_KEY=sk-xxxx python3 tools/deepseek_proxy.py

    # Point at a different OpenAI-compatible host
    python3 tools/deepseek_proxy.py --host api.openai.com --path /v1/chat/completions

    # Force a model name (so the guest can keep its default "nexos" model)
    python3 tools/deepseek_proxy.py --force-model deepseek-chat

    # For local end-to-end testing (no external network):
    #   start a local HTTPS mock and point the proxy at it
    python3 tools/deepseek_proxy.py --host 127.0.0.1 --port 8443 \
        --cert test_cert.pem --key test_key.pem --insecure

The guest then sets, in the /agent web console or via:
    agent remote-url http://10.0.2.2:18999/v1/chat/completions
    agent remote-key  <your DeepSeek key>      # optional if proxy holds DEEPSEEK_API_KEY
    agent remote-model deepseek-chat           # optional if --force-model is set
    agent remote "Hello from NexOS"
"""
import os
import sys
import json
import ssl
import http.client
import urllib.parse
from http.server import BaseHTTPRequestHandler, HTTPServer

import argparse

# ---- configuration (env overrides, then CLI overrides) ---------------------
def env_or(name, default):
    return os.environ.get(name, default)

def main():
    ap = argparse.ArgumentParser(description="NexOS agent -> DeepSeek TLS bridge")
    ap.add_argument("--listen", default=env_or("LISTEN_PORT", "18999"),
                    help="plain-HTTP listen port on the host (default 18999)")
    ap.add_argument("--listen-host", default="0.0.0.0",
                    help="plain-HTTP bind address (default 0.0.0.0)")
    ap.add_argument("--host", default=env_or("DEEPSEEK_HOST", "api.deepseek.com"),
                    help="upstream HTTPS host (default api.deepseek.com)")
    ap.add_argument("--port", default=env_or("DEEPSEEK_PORT", "443"),
                    help="upstream HTTPS port (default 443)")
    ap.add_argument("--path", default=env_or("DEEPSEEK_PATH", "/v1/chat/completions"),
                    help="upstream request path (default /v1/chat/completions)")
    ap.add_argument("--key", default=env_or("DEEPSEEK_API_KEY", ""),
                    help="API key injected if the guest omits Authorization")
    ap.add_argument("--force-model", default=env_or("DEEPSEEK_MODEL", ""),
                    help="if set, rewrite the request body's model field to this")
    ap.add_argument("--insecure", action="store_true",
                    help="do not verify the upstream TLS certificate (local mocks)")
    ap.add_argument("--cert", default=env_or("DEEPSEEK_TLS_CERT", ""),
                    help="client cert for the upstream (rarely needed)")
    ap.add_argument("--keyfile", default=env_or("DEEPSEEK_TLS_KEY", ""),
                    help="client key for the upstream (rarely needed)")
    args = ap.parse_args()

    LISTEN_PORT = int(args.listen)
    UP_HOST = args.host
    UP_PORT = int(args.port)
    UP_PATH = args.path
    API_KEY = args.key
    FORCE_MODEL = args.force_model
    INSECURE = args.insecure

    print("[deepseek_proxy] listening on http://%s:%d" % (args.listen_host, LISTEN_PORT))
    print("[deepseek_proxy] forwarding HTTPS -> https://%s:%d%s" % (UP_HOST, UP_PORT, UP_PATH))
    if API_KEY:
        print("[deepseek_proxy] API key: %s...%s" % (API_KEY[:6], API_KEY[-4:]))
    if FORCE_MODEL:
        print("[deepseek_proxy] force model -> %s" % FORCE_MODEL)
    if INSECURE:
        print("[deepseek_proxy] TLS verification DISABLED (insecure)")

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def _relay(self):
            # Read the guest's request body.
            length = int(self.headers.get("Content-Length", "0") or "0")
            body = self.rfile.read(length) if length else b""

            # Optionally rewrite the model name so the guest can keep its
            # default "nexos" model while still hitting DeepSeek.
            if FORCE_MODEL and body:
                try:
                    obj = json.loads(body.decode("utf-8", "ignore"))
                    if isinstance(obj, dict) and "model" in obj:
                        obj["model"] = FORCE_MODEL
                        body = json.dumps(obj).encode("utf-8")
                except Exception:
                    pass

            # Build upstream headers from the guest request, preserving the
            # Authorization header the guest sent (its own DeepSeek key).
            fwd = {}
            for k in ("Content-Type", "Authorization", "Accept", "User-Agent"):
                if k in self.headers:
                    fwd[k] = self.headers[k]
            if "Content-Type" not in fwd:
                fwd["Content-Type"] = "application/json"
            if API_KEY and "Authorization" not in fwd:
                fwd["Authorization"] = "Bearer " + API_KEY

            # TLS context.
            ctx = ssl.create_default_context()
            if INSECURE:
                ctx.check_hostname = False
                ctx.verify_mode = ssl.CERT_NONE

            try:
                conn = http.client.HTTPSConnection(
                    UP_HOST, UP_PORT, context=ctx, timeout=30)
                # Use the path the guest requested, falling back to the
                # configured upstream path.
                up_path = self.path if self.path.startswith("/") else UP_PATH
                if up_path == "/" or up_path == "":
                    up_path = UP_PATH
                conn.request(self.command, up_path, body=body, headers=fwd)
                resp = conn.getresponse()
                data = resp.read()
                conn.close()

                self.send_response(resp.status)
                ct = resp.getheader("Content-Type", "application/json")
                self.send_header("Content-Type", ct)
                self.send_header("Content-Length", str(len(data)))
                self.send_header("Access-Control-Allow-Origin", "*")
                self.end_headers()
                self.wfile.write(data)
            except Exception as e:
                msg = json.dumps({"error": "proxy upstream failed: %s" % e}).encode()
                self.send_response(502)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(msg)))
                self.send_header("Access-Control-Allow-Origin", "*")
                self.end_headers()
                self.wfile.write(msg)
                print("[deepseek_proxy] upstream error: %s" % e)

        def do_POST(self):
            self._relay()

        def do_GET(self):
            # Health check.
            msg = json.dumps({"status": "ok",
                              "upstream": "https://%s:%d%s" % (UP_HOST, UP_PORT, UP_PATH)}).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(msg)))
            self.send_header("Access-Control-Allow-Origin", "*")
            self.end_headers()
            self.wfile.write(msg)

        def do_OPTIONS(self):
            self.send_response(204)
            self.send_header("Access-Control-Allow-Origin", "*")
            self.send_header("Access-Control-Allow-Headers", "Content-Type, Authorization")
            self.end_headers()

    srv = HTTPServer((args.listen_host, LISTEN_PORT), Handler)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("\n[deepseek_proxy] stopped")
    finally:
        srv.server_close()


if __name__ == "__main__":
    main()
