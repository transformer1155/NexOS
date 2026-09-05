#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Host-side LLM bridge for NexOS (Windows, no WSL).

NexOS's micro TCP/IP stack has an HTTP GET client but no TLS and no POST, so
it cannot talk to LM Studio / Ollama directly (both need POST + JSON).  This
service translates the GET request NexOS can make into the POST the local LLM
server expects:

    NexOS:  GET http://10.0.2.2:18080/ask?q=<url-encoded question>
    Bridge: POST http://127.0.0.1:<port>/v1/chat/completions  (LM Studio,
            OpenAI-compatible)  -- or /api/generate (Ollama)
    Reply:  text/plain answer body

The QEMU user network maps the host as 10.0.2.2, so the guest reaches this
service without any hostfwd.  Start it with:

    python tools/llm_bridge.py [--port 18080] [--lm http://127.0.0.1:1234]
                               [--backend lmstudio|ollama]
"""
import argparse
import json
import sys
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

PARSER = argparse.ArgumentParser(description="NexOS <-> local LLM bridge")
PARSER.add_argument("--port", type=int, default=18080)
PARSER.add_argument("--lm", default="http://127.0.0.1:1234",
                    help="local LLM server base URL (LM Studio default 1234, Ollama 11434)")
PARSER.add_argument("--backend", choices=["lmstudio", "ollama"], default="lmstudio")
PARSER.add_argument("--model", default="", help="explicit model id (default: first from /v1/models)")
ARGS = PARSER.parse_args()


def pick_model():
    if ARGS.model:
        return ARGS.model
    if ARGS.backend == "ollama":
        with urllib.request.urlopen(ARGS.lm + "/api/tags", timeout=5) as r:
            d = json.load(r)
        for m in d.get("models", []):
            return m.get("name") or m.get("model") or ""
    else:
        with urllib.request.urlopen(ARGS.lm + "/v1/models", timeout=5) as r:
            d = json.load(r)
        for m in d.get("data", []):
            return m.get("id") or m.get("name") or ""
    return ""


def ask(q, model):
    if ARGS.backend == "ollama":
        body = json.dumps({"model": model, "prompt": q, "stream": False,
                           "options": {"num_predict": 512}}).encode()
        req = urllib.request.Request(ARGS.lm + "/api/generate", data=body,
                                     headers={"Content-Type": "application/json"})
        with urllib.request.urlopen(req, timeout=300) as r:
            d = json.load(r)
        return (d.get("response") or "").strip()
    body = json.dumps({"model": model,
                       "messages": [{"role": "user", "content": q}],
                       "stream": False, "max_tokens": 512}).encode()
    req = urllib.request.Request(ARGS.lm + "/v1/chat/completions", data=body,
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=300) as r:
        d = json.load(r)
    try:
        return d["choices"][0]["message"]["content"].strip()
    except Exception:
        return json.dumps(d)[:500]


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        u = urlparse(self.path)
        if u.path == "/ask":
            q = (parse_qs(u.query).get("q", [""])[0] or "").strip()
            if not q:
                self._send(400, b"empty question")
                return
            try:
                model = pick_model()
                if not model:
                    self._send(503, b"no model loaded in the local LLM server")
                    return
                ans = ask(q, model)
                self._send(200, ans.encode("utf-8", "replace"))
            except Exception as e:  # noqa
                self._send(500, ("ERR " + str(e)).encode("utf-8", "replace"))
            return
        self._send(404, b"not found")

    def _send(self, code, data):
        self.send_response(code)
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, *a):  # keep the console quiet
        pass


def main():
    model = ""
    try:
        model = pick_model()
    except Exception:
        pass
    srv = ThreadingHTTPServer(("0.0.0.0", ARGS.port), Handler)
    print("NexOS LLM bridge listening on :%d (backend=%s lm=%s model=%r)"
          % (ARGS.port, ARGS.backend, ARGS.lm, model), flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
