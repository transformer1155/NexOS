#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""opencode_free.py -- keyless client for OpenCode Zen free models.

WHY THIS EXISTS
---------------
OpenCode Zen exposes an OpenAI-compatible endpoint.  Its free models accept the
literal sentinel key "public" -- no account, no OAuth, no token refresh.  This
module wraps that endpoint so the NexOS host bridges (gateway_server.py,
llm_bridge.py, deepseek_proxy.py) can answer AI requests with zero setup.

    POST https://opencode.ai/zen/v1/chat/completions
    Authorization: Bearer public
    {"model": "<free-model>", "messages": [...]}

CAVEATS (verified 2026-10-31)
-----------------------------
* "public" is a SHARED ANONYMOUS key.  It is rate limited server-side per IP;
  an exhausted window answers HTTP 429 {"type":"error","error":{"type":
  "FreeUsageLimitError","message":"Free usage limit reached..."}} plus a
  Retry-After header (observed ~19h).
* The free model set rotates without notice.  GET /zen/v1/models with the same
  sentinel lists every model; this module keeps only cost==0 entries.
* Set OPENCODE_ZEN_KEY to a real key (still needs an account) or OPENCODE_ZEN_BASE
  to point at a proxy if you want more quota.

USAGE
-----
    from opencode_free import chat, list_free_models, format_failure
    ok, text, meta = chat("你好")
    if not ok: print(format_failure(meta))
"""
import json
import os
import time
import urllib.error
import urllib.request

ZEN_BASE = os.environ.get("OPENCODE_ZEN_BASE", "https://opencode.ai/zen/v1").rstrip("/")
ZEN_KEY = os.environ.get("OPENCODE_ZEN_KEY", "public")

# Preferred order.  Kept small and well-tested; unknown/rotated ids are skipped
# automatically because /models is consulted first when it is reachable.
PREFERRED_MODELS = [
    "deepseek-v4.1-flash-free",
    "glm-5.3-flash-free",
    "big-pickle",
    "mimo-v2.6-flash-free",
    "flint-40b-free",
    "nemotron-3.5-lightning-free",
]
# Used when the model catalog cannot be fetched (offline cache / cold start).
FALLBACK_MODELS = list(PREFERRED_MODELS)

_CACHE = {"models": None, "ts": 0.0}
CACHE_TTL = 3600.0          # 1h; the free set changes rarely


def _request(url, data=None, headers=None, timeout=60):
    req = urllib.request.Request(url, data=data, headers=headers or {},
                                 method="POST" if data is not None else "GET")
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.status, r.read()


def _auth_headers(extra=None):
    h = {"Authorization": "Bearer " + ZEN_KEY,
         "Content-Type": "application/json",
         "User-Agent": "NexOS-opencode-free/1.0"}
    if extra:
        h.update(extra)
    return h


def list_free_models(timeout=10):
    """Return [(id, name)] of models whose input AND output cost is 0.

    Cached for CACHE_TTL seconds.  Returns [] on any failure (caller should
    fall back to PREFERRED_MODELS).
    """
    now = time.time()
    if _CACHE["models"] is not None and now - _CACHE["ts"] < CACHE_TTL:
        return list(_CACHE["models"])
    try:
        status, raw = _request(ZEN_BASE + "/models", headers=_auth_headers(),
                               timeout=timeout)
        if status != 200:
            return []
        data = json.loads(raw.decode("utf-8", "ignore"))
        out = []
        for m in data.get("data", []):
            try:
                cost = m.get("cost") or {}
                txt = cost.get("text") or {}
                ci = (txt.get("input") or {}).get("price")
                co = (txt.get("output") or {}).get("price")
            except AttributeError:
                continue
            if ci == 0 and co == 0:
                out.append((m.get("id", ""), m.get("name", m.get("id", ""))))
        out = [x for x in out if x[0]]
        if out:
            # Preferred first, then the rest in catalog order.
            pref = {m: i for i, m in enumerate(PREFERRED_MODELS)}
            out.sort(key=lambda x: (pref.get(x[0], 999), x[0]))
            _CACHE["models"] = list(out)
            _CACHE["ts"] = now
        return out
    except Exception:
        return []


def candidate_models(model=None):
    """Model ids to try, best first: explicit, preferred, live catalog."""
    seen, out = set(), []

    def add(mid):
        if mid and mid not in seen:
            seen.add(mid)
            out.append(mid)

    add(model or os.environ.get("OPENCODE_ZEN_MODEL", ""))
    for m in PREFERRED_MODELS:
        add(m)
    for mid, _name in list_free_models():
        add(mid)
    for m in FALLBACK_MODELS:
        add(m)
    return out


def chat(prompt, model=None, system=None, max_tokens=512, temperature=0.7,
         timeout=60, retries=1):
    """Ask a free OpenCode Zen model.

    Returns (ok, text, meta).  meta always carries:
      model      -- the model that answered (or was tried last)
      attempts   -- list of {model, status, error}
      retry_after-- seconds from the last 429 Retry-After, if any
      rate_limited -- True when every attempt was 429
    On failure text is a short human-readable reason (ASCII-safe).
    """
    messages = []
    if system:
        messages.append({"role": "system", "content": system})
    messages.append({"role": "user", "content": prompt or ""})

    attempts = []
    retry_after = None
    last_err = "no model available"
    models = candidate_models(model)
    if not models:
        return False, "no free model available", {"attempts": attempts,
                                                  "rate_limited": False,
                                                  "retry_after": None,
                                                  "model": None}

    # Keep the sweep bounded: a stale catalog can be long, and every 429 costs
    # a round trip.  Two passes over the first few ids is plenty.
    sweep = models[:4] if len(models) > 4 else models
    for attempt in range(max(1, retries)):
        for mid in sweep:
            body = json.dumps({
                "model": mid,
                "messages": messages,
                "max_tokens": int(max_tokens),
                "temperature": float(temperature),
                "stream": False,
            }).encode("utf-8")
            try:
                status, raw = _request(ZEN_BASE + "/chat/completions",
                                       data=body, headers=_auth_headers(),
                                       timeout=timeout)
            except urllib.error.HTTPError as e:
                status = e.code
                raw = e.read() if hasattr(e, "read") else b""
                ra = e.headers.get("Retry-After") if e.headers else None
                if ra:
                    try: retry_after = int(float(ra))
                    except ValueError: pass
                err = _error_text(raw)
                attempts.append({"model": mid, "status": status, "error": err})
                last_err = err or ("HTTP %d" % status)
                # 429 on one model is normally a global window -> try the next
                # id once, then stop early instead of hammering N models.
                if status == 429 and len(attempts) >= 2:
                    break
                continue
            except Exception as e:                      # network, TLS, DNS...
                attempts.append({"model": mid, "status": 0, "error": str(e)})
                last_err = str(e)
                continue

            if status != 200:
                err = _error_text(raw)
                attempts.append({"model": mid, "status": status, "error": err})
                last_err = err or ("HTTP %d" % status)
                continue

            try:
                data = json.loads(raw.decode("utf-8", "ignore"))
                content = data["choices"][0]["message"]["content"]
            except Exception:
                attempts.append({"model": mid, "status": status,
                                 "error": "unparseable response"})
                last_err = "unparseable response"
                continue
            attempts.append({"model": mid, "status": 200, "error": ""})
            return True, (content or ""), {
                "model": mid, "attempts": attempts,
                "rate_limited": False, "retry_after": retry_after,
            }

    rate_limited = bool(attempts) and all(a["status"] == 429 for a in attempts)
    if rate_limited:
        return False, "OpenCode free quota exhausted", {
            "model": attempts[-1]["model"] if attempts else None,
            "attempts": attempts, "rate_limited": True,
            "retry_after": retry_after,
        }
    return False, last_err, {
        "model": attempts[-1]["model"] if attempts else None,
        "attempts": attempts, "rate_limited": False,
        "retry_after": retry_after,
    }


def _error_text(raw):
    try:
        obj = json.loads(raw.decode("utf-8", "ignore"))
        err = obj.get("error")
        if isinstance(err, dict):
            return str(err.get("message") or err.get("type") or "")
        if isinstance(err, str):
            return err
        if isinstance(obj.get("message"), str):
            return obj["message"]
    except Exception:
        pass
    try:
        return raw.decode("utf-8", "ignore")[:200]
    except Exception:
        return ""


def format_failure(meta):
    """Short ASCII-safe explanation for logs / HTTP bodies."""
    if meta.get("rate_limited"):
        ra = meta.get("retry_after")
        if ra:
            return "OpenCode free quota exhausted (retry in ~%d min)" % (ra // 60)
        return "OpenCode free quota exhausted"
    return "OpenCode request failed: " + str(meta.get("attempts", [{}])[-1]
                                             .get("error", "unknown"))


if __name__ == "__main__":
    import sys
    q = " ".join(sys.argv[1:]) or "Say OK"
    ok, text, meta = chat(q, max_tokens=32)
    print("ok=", ok)
    print("text=", text)
    print("meta=", json.dumps(meta, ensure_ascii=False))
