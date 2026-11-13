#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""NexOS AI platform registry -- HOST SIDE ONLY.

This module is the single mechanism behind:
  * an OpenAI-compatible /v1/chat/completions + /v1/models protocol,
  * centrally HOSTED API keys (keys never enter the guest image),
  * per-platform AND per-app quota ledgers with daily reset + persistence,
  * applications contributing their own models to the shared catalogue.

SECURITY: the guest (kernel / MiniCLR / .mex apps) only ever sends a model
name and a request body.  Authorization headers are added here from
environment variables or from ai_platforms.local.json, which is gitignored
and never packed into sfs_files/.  No key is ever returned to the guest.

Offline verification: set NEXOS_AI_OFFLINE=1 to answer locally without any
upstream network access (same response shape as a real OpenAI endpoint).
"""
import json
import os
import threading
import time
import urllib.error
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
CONFIG_PATH = os.path.join(HERE, "ai_platforms.json")
LOCAL_PATH = os.path.join(HERE, "ai_platforms.local.json")   # optional, gitignored
MODELS_PATH = os.path.join(HERE, "ai_models.json")           # app-added, gitignored
USAGE_PATH = os.path.join(HERE, "ai_usage.json")             # ledger, gitignored
APPS_PATH = os.path.join(os.path.dirname(os.path.dirname(HERE)),
                         "sfs_files", "apps.json")

_lock = threading.RLock()
_cfg = None
_app_models = None
_usage_cache = None


# ---------------------------------------------------------------- helpers
def _read_json(path, default):
    try:
        with open(path, "r", encoding="utf-8") as fh:
            return json.load(fh)
    except (OSError, ValueError):
        return default


def _write_json(path, obj):
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as fh:
        json.dump(obj, fh, ensure_ascii=False, indent=2)
        fh.write("\n")
    os.replace(tmp, path)


def _today():
    return time.strftime("%Y-%m-%d", time.gmtime())


def _offline():
    return os.environ.get("NEXOS_AI_OFFLINE", "").strip().lower() in ("1", "true", "yes")


def _config():
    global _cfg
    with _lock:
        if _cfg is None:
            cfg = _read_json(CONFIG_PATH, {})
            local = _read_json(LOCAL_PATH, {})       # host-only key/quota override
            for over in local.get("platforms", []) or []:
                for row in cfg.get("platforms", []) or []:
                    if row.get("id") == over.get("id"):
                        row.update(over)
            if local:
                cfg["local_override"] = True
            _cfg = cfg
        return _cfg


def _models():
    global _app_models
    with _lock:
        if _app_models is None:
            _app_models = _read_json(MODELS_PATH, [])
        return _app_models


def _usage():
    global _usage_cache
    with _lock:
        if _usage_cache is None:
            u = _read_json(USAGE_PATH, {})
            if not isinstance(u, dict) or u.get("day") != _today():
                u = {"day": _today(), "platforms": {}, "apps": {}}
            _usage_cache = u
        return _usage_cache


# ---------------------------------------------------------------- registry
def platforms():
    return _config().get("platforms", []) or []


def platform_by_id(pid):
    for row in platforms():
        if row.get("id") == pid:
            return row
    return None


def default_platform_id():
    return _config().get("default_platform", "opencode")


def resolve_key(row):
    """Hosted key: environment first, then host-only local override.
    Never serialised into a guest-visible config."""
    env = row.get("key_env") or ""
    if env and os.environ.get(env):
        return os.environ[env]
    return row.get("key", "") or ""


def key_report():
    """Masked inventory -- proves keys are hosted without revealing them."""
    out = []
    for row in platforms():
        k = resolve_key(row)
        hint = (k[:3] + "***" + k[-2:]) if len(k) > 8 else ("set" if k else "unset")
        out.append({"id": row.get("id"), "key_env": row.get("key_env", ""),
                    "key_present": bool(k), "key_hint": hint})
    return out


def _catalogue():
    """Builtin models + models declared in apps.json + app-added models."""
    cat = []
    for row in platforms():
        for m in row.get("models", []) or []:
            if isinstance(m, str):
                cat.append({"id": m, "platform": row["id"], "owned_by": "system"})
            elif isinstance(m, dict) and m.get("id"):
                d = dict(m)
                d.setdefault("platform", row["id"])
                d.setdefault("owned_by", "system")
                cat.append(d)
    for m in _read_json(APPS_PATH, {}).get("models", []) or []:
        if isinstance(m, dict) and m.get("id"):
            d = dict(m)
            d.setdefault("owned_by", "apps.json")
            cat.append(d)
    for m in _models():
        if isinstance(m, dict) and m.get("id"):
            d = dict(m)
            d.setdefault("owned_by", d.get("app_id", "app"))
            cat.append(d)
    # de-duplicate by id; later (app-added) entries win
    seen = {}
    for d in cat:
        seen[d["id"]] = d
    return list(seen.values())


def list_models():
    """OpenAI GET /v1/models shape."""
    data = [{"id": m["id"], "object": "model", "created": 0,
             "owned_by": m.get("owned_by", "system"),
             "platform": m.get("platform", "")}
            for m in _catalogue()]
    return {"object": "list", "data": data}


def add_model(spec, app_id=""):
    """An application registers a model into the shared catalogue.

    spec may be a JSON string or dict: {"id":"...","platform":"...","name":"..."}.
    The registration is persisted on the HOST (ai_models.json).
    """
    if isinstance(spec, bytes):
        spec = spec.decode("utf-8", "replace")
    if isinstance(spec, str):
        try:
            spec = json.loads(spec)
        except ValueError:
            return {"ok": False, "error": "bad json"}
    if not isinstance(spec, dict) or not spec.get("id"):
        return {"ok": False, "error": "model id required"}
    rec = {"id": str(spec["id"]),
           "platform": str(spec.get("platform") or default_platform_id()),
           "name": str(spec.get("name") or spec["id"]),
           "app_id": str(app_id or spec.get("app_id") or "unknown"),
           "owned_by": str(app_id or spec.get("app_id") or "app"),
           "added_at": int(time.time())}
    with _lock:
        rows = _models()
        for i, m in enumerate(rows):
            if m.get("id") == rec["id"]:
                rows[i] = rec
                break
        else:
            rows.append(rec)
        _write_json(MODELS_PATH, rows)
    return {"ok": True, "model": rec}


# ---------------------------------------------------------------- quota
def _limits(pid):
    q = (platform_by_id(pid) or {}).get("quota", {}) or {}
    return int(q.get("daily_requests", 0) or 0), int(q.get("daily_tokens", 0) or 0)


def _app_limit(app_id):
    q = _config().get("quota", {}) or {}
    default = int(q.get("per_app_default", {}).get("daily_requests", 0) or 0)
    return int((q.get("per_app", {}) or {}).get(app_id, default) or 0)


def _roll_if_needed(u):
    if u.get("day") != _today():
        u.clear()
        u.update({"day": _today(), "platforms": {}, "apps": {}})


def quota_check(pid, app_id=""):
    """-> (allowed, reason).  Checks platform requests/tokens and app requests."""
    with _lock:
        u = _usage()
        _roll_if_needed(u)
        pr, pt = _limits(pid)
        row = u["platforms"].setdefault(pid, {"requests": 0, "tokens": 0})
        if pr and row.get("requests", 0) >= pr:
            return False, "platform %s daily request quota exhausted" % pid
        if pt and row.get("tokens", 0) >= pt:
            return False, "platform %s daily token quota exhausted" % pid
        al = _app_limit(app_id)
        arow = u["apps"].setdefault(app_id or "_kernel", {"requests": 0})
        if al and arow.get("requests", 0) >= al:
            return False, "app %s daily quota exhausted" % (app_id or "_kernel")
        return True, ""


def quota_charge(pid, app_id="", tokens=0):
    with _lock:
        u = _usage()
        _roll_if_needed(u)
        row = u["platforms"].setdefault(pid, {"requests": 0, "tokens": 0})
        row["requests"] = row.get("requests", 0) + 1
        row["tokens"] = row.get("tokens", 0) + int(tokens or 0)
        arow = u["apps"].setdefault(app_id or "_kernel", {"requests": 0})
        arow["requests"] = arow.get("requests", 0) + 1
        _write_json(USAGE_PATH, u)


def quota_report():
    """Full ledger.  `total`/`remaining` keep the old /quota callers working."""
    with _lock:
        u = _usage()
        _roll_if_needed(u)
        out = {"day": u.get("day"),
               "reset": (_config().get("quota", {}) or {}).get("reset", "daily"),
               "key_vault": key_report(),
               "platforms": {}, "apps": dict(u.get("apps", {}))}
        for row in platforms():
            pid = row["id"]
            pr, pt = _limits(pid)
            used = u.get("platforms", {}).get(pid, {"requests": 0, "tokens": 0})
            q = row.get("quota", {}) or {}
            out["platforms"][pid] = {
                "enabled": q.get("enabled", True),
                "daily_requests": pr, "used_requests": used.get("requests", 0),
                "remaining_requests": max(0, pr - used.get("requests", 0)) if pr else -1,
                "daily_tokens": pt, "used_tokens": used.get("tokens", 0),
                "key_present": bool(resolve_key(row)),
            }
    total = sum(v["remaining_requests"] for v in out["platforms"].values()
                if v["remaining_requests"] >= 0)
    out["total"] = total
    out["remaining"] = total
    return out


# ---------------------------------------------------------------- chat
def _last_user_text(payload):
    for m in reversed(payload.get("messages") or []):
        if isinstance(m, dict) and m.get("role") == "user":
            c = m.get("content")
            if isinstance(c, str):
                return c[:400]
    return str(payload.get("prompt", ""))[:400]


def _completion(model, pid, text):
    return {"id": "chatcmpl-nexos-" + str(int(time.time() * 1000)),
            "object": "chat.completion", "created": int(time.time()),
            "model": model, "platform": pid,
            "choices": [{"index": 0,
                         "message": {"role": "assistant", "content": text},
                         "finish_reason": "stop"}],
            "usage": {"prompt_tokens": len(text),
                      "completion_tokens": len(text),
                      "total_tokens": len(text) * 2}}


def chat_completions(payload, app_id=""):
    """OpenAI-compatible POST /v1/chat/completions.

    Routes by model -> platform, injects the HOSTED key, enforces quota,
    forwards upstream, and records usage.  Returns (http_status, response).
    """
    if not isinstance(payload, dict):
        return 400, {"error": {"message": "bad request body",
                               "type": "invalid_request_error"}}
    model = str(payload.get("model") or "")
    hit = next((m for m in _catalogue() if m.get("id") == model), None)
    pid = ((hit or {}).get("platform") or payload.get("platform")
           or default_platform_id())
    row = platform_by_id(pid)
    if not row:
        return 400, {"error": {"message": "unknown platform: " + str(pid),
                               "type": "invalid_request_error"}}
    if not (row.get("quota", {}) or {}).get("enabled", True):
        return 403, {"error": {"message": "platform disabled: " + str(pid),
                               "type": "permission_error"}}
    ok, why = quota_check(pid, app_id)
    if not ok:
        return 429, {"error": {"message": why, "type": "rate_limit_error"},
                     "quota": quota_report()}

    if not model:
        first = (row.get("models") or ["nexos-local"])[0]
        model = first.get("id", "nexos-local") if isinstance(first, dict) else first
        payload = dict(payload)
        payload["model"] = model

    if _offline() or row.get("type") in ("echo", "local-gguf"):
        status, resp = 200, _completion(model, pid,
                                        "(offline echo) " + _last_user_text(payload))
    else:
        base = (row.get("base_url") or "").rstrip("/")
        headers = {"Content-Type": "application/json"}
        key = resolve_key(row)
        if key:
            headers["Authorization"] = "Bearer " + key
        body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        try:
            req = urllib.request.Request(base + "/chat/completions", data=body,
                                         headers=headers, method="POST")
            with urllib.request.urlopen(req, timeout=int(row.get("timeout", 60))) as r:
                status = r.status
                resp = json.loads(r.read().decode("utf-8", "replace"))
        except urllib.error.HTTPError as e:
            status = e.code
            try:
                resp = json.loads(e.read().decode("utf-8", "replace"))
            except Exception:
                resp = {"error": {"message": "upstream HTTP %d" % e.code,
                                  "type": "upstream_error"}}
        except Exception as e:
            status, resp = 502, {"error": {"message": "upstream unreachable: " + str(e),
                                           "type": "upstream_error"}}

    if status == 200:
        try:
            tokens = int((resp.get("usage") or {}).get("total_tokens", 0))
        except (AttributeError, ValueError, TypeError):
            tokens = 0
        quota_charge(pid, app_id, tokens)
        resp["nexos"] = {"platform": pid, "app": app_id,
                         "quota": quota_report()["platforms"].get(pid, {})}
    return status, resp
