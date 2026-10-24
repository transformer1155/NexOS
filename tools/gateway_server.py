#!/usr/bin/env python3
# =====================================================================
#  gateway_server.py  -  宿主侧 HTTP gateway (端口 19387)
# ---------------------------------------------------------------------
#  NexOS kernel (MiniCLR on Guest) 调用 Backend.cs 里的 HttpClient 发 HTTP 请求
#  到 127.0.0.1:19387。本脚本是这个 HTTP server 的 Python 实现。
#
#  支持的端点:
#    POST /package/install   接收 .nexpack 二进制流 → 调用 PackageInstaller
#    GET  /package/status    查询已安装包列表
#    POST /quota             AI quota 查询
#    POST /ai                AI 生成请求
#    POST /mcp/<app_id>      MCP tool dispatch
#
#  依赖: Python 3.8+ 标准库 (http.server + socket + struct)
#  启动: python3 tools/gateway_server.py [--port 19387]
# =====================================================================
import argparse, hashlib, json, os, socket, sys, threading, time, zipfile

from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)

# ---- 配置 ---------------------------------------------------------------
PKG_DIR    = os.environ.get("NEXOS_PKG_DIR",
              os.path.join(REPO, "mkfs"))          # 安装目标目录 (Host 侧)
APPS_JSON  = os.path.join(REPO, "sfs_files", "apps.json")
QUOTA_TOTAL = 1000                                  # 初始 AI quota
quota_left  = QUOTA_TOTAL                            # 运行时剩余
quota_lock  = threading.Lock()

INSTALLED = {}    # app_id -> { path, sha256, size, installed_at }
INSTALLED_LOCK = threading.Lock()

MAX_PACKAGE_BYTES = 4 * 1024 * 1024                 # 4 MB 上限
MAX_MEX_BYTES     = 2 * 1024 * 1024


# ---- 全局配置 -----------------------------------------------------------
_PACKAGE_DIR = PKG_DIR   # module-level backing (set_pkg_dir 写它)

def set_pkg_dir(d):
    global _PACKAGE_DIR
    _PACKAGE_DIR = d


# ---- 包解析/校验 ---------------------------------------------------------
def sha256_of(data):
    return hashlib.sha256(data).hexdigest()


def parse_nexpack(data):
    """解析 .nexpack 二进制 -> (manifest_dict, app.mex_bytes, errors)."""
    errors = []
    try:
        import io
        z = zipfile.ZipFile(io.BytesIO(data), "r")
    except zipfile.BadZipFile as e:
        return None, None, ["bad zip: " + str(e)]

    for info in z.infolist():
        if info.compress_type != zipfile.ZIP_STORED:
            errors.append("member '%s' is DEFLATE (must be STORED)" % info.filename)

    if "manifest.json" not in z.namelist():
        errors.append("missing manifest.json")
    if "app.mex" not in z.namelist():
        errors.append("missing app.mex")

    if errors: return None, None, errors

    manifest = json.loads(z.read("manifest.json"))
    mex      = z.read("app.mex")

    # 校验 manifest
    if manifest.get("schema_version") != 1:
        errors.append("schema_version must be 1")
    for k in ("app_id", "name", "version", "author"):
        if not manifest.get(k):
            errors.append("manifest missing: " + k)

    # 校验 mex 魔数
    if len(mex) < 4 or mex[:4] != b"MEX1":
        errors.append("app.mex bad magic (want MEX1)")
    if len(mex) > MAX_MEX_BYTES:
        errors.append("app.mex > %d bytes" % MAX_MEX_BYTES)

    return manifest, mex, errors


def install_nexpack(data):
    """把 .nexpack 数据安装到 _PACKAGE_DIR。返回 (ok, message, report)."""
    # 1. 体积
    if len(data) > MAX_PACKAGE_BYTES:
        return False, "package > %d bytes" % MAX_PACKAGE_BYTES, {}

    # 2. 解析
    manifest, mex, errs = parse_nexpack(data)
    if errs:
        return False, "; ".join(errs), {}

    app_id = manifest["app_id"]
    sha    = sha256_of(data)

    base = _PACKAGE_DIR
    # 3. 落盘
    os.makedirs(base, exist_ok=True)
    os.makedirs(os.path.join(base, "Desktop"), exist_ok=True)

    mex_path = os.path.join(base, app_id + ".mex")
    with open(mex_path, "wb") as f:
        f.write(mex)

    # icon (可选)
    try:
        import io
        with zipfile.ZipFile(io.BytesIO(data)) as z:
            icon_name = manifest.get("icon")
            if icon_name and icon_name in z.namelist():
                icon_path = os.path.join(base, app_id + ".icon")
                with open(icon_path, "wb") as f:
                    f.write(z.read(icon_name))
    except: pass

    # 4. 写 apps.overlay.json
    overlay_path = os.path.join(base, "apps.overlay.json")
    overlay = {"schema_version": 1, "apps": []}
    if os.path.exists(overlay_path):
        try: overlay = json.loads(open(overlay_path, encoding="utf-8").read())
        except: pass
    # upsert
    new_entry = {"app_id": app_id, "name": manifest["name"],
                 "version": manifest["version"], "author": manifest["author"]}
    overlay["apps"] = [a for a in overlay.get("apps", []) if a.get("app_id") != app_id]
    overlay["apps"].append(new_entry)
    with open(overlay_path, "w", encoding="utf-8") as f:
        json.dump(overlay, f, indent=2, ensure_ascii=False)

    # 5. 记录
    with INSTALLED_LOCK:
        INSTALLED[app_id] = {
            "path": mex_path, "sha256": sha,
            "size": len(data), "installed_at": time.time()
        }

    report = {
        "app_id": app_id, "version": manifest["version"],
        "author": manifest["author"], "mex_size": len(mex),
        "sha256": sha, "pkg_size": len(data)
    }
    return True, "installed", report


# ---- HTTP handler --------------------------------------------------------
class GatewayHandler(BaseHTTPRequestHandler):
    server_version = "NexOS-Gateway/1.0"

    def log_message(self, fmt, *args):
        sys.stderr.write("[gw] " + (fmt % args) + "\n")

    def _send_json(self, code, obj):
        body = json.dumps(obj).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _send_plain(self, code, text):
        body = text.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _read_body(self):
        n = int(self.headers.get("Content-Length", "0"))
        if n == 0: return b""
        return self.rfile.read(n)

    # ---- POST endpoints ----
    def do_POST(self):
        path = self.path.split("?")[0]

        # /package/install
        if path == "/package/install":
            return self._handle_package_install()

        # /ai
        if path == "/ai":
            return self._handle_ai()

        # /quota
        if path == "/quota":
            return self._handle_quota()

        # /mcp/<app_id>
        if path.startswith("/mcp/"):
            return self._handle_mcp(path[5:])

        self._send_json(404, {"error": "not found: " + path})

    def _handle_package_install(self):
        """POST /package/install — 接收 .nexpack 二进制流。"""
        data = self._read_body()
        if len(data) == 0:
            return self._send_json(400, {"error": "empty body"})
        if len(data) > MAX_PACKAGE_BYTES:
            return self._send_json(413, {"error": "too large (%d > %d)" % (len(data), MAX_PACKAGE_BYTES)})

        ok, msg, report = install_nexpack(data)
        if ok:
            self._send_json(200, {"ok": True, "message": msg, "report": report})
        else:
            self._send_json(400, {"ok": False, "error": msg})

    def _handle_ai(self):
        body = self._read_body()
        try: payload = json.loads(body.decode("utf-8"))
        except: return self._send_json(400, {"error": "bad json"})
        prompt = payload.get("prompt", "")
        with quota_lock:
            if quota_left <= 0:
                return self._send_json(429, {"error": "quota exhausted"})
            quota_left -= 1
        return self._send_json(200, {"ok": True, "reply": "(host ai echo) " + prompt[:200]})

    def _handle_quota(self):
        with quota_lock:
            return self._send_json(200, {
                "total": QUOTA_TOTAL, "remaining": quota_left
            })

    def _handle_mcp(self, target_app_id):
        body = self._read_body()
        try: payload = json.loads(body.decode("utf-8"))
        except: return self._send_json(400, {"error": "bad json"})
        tool = payload.get("tool_name", "")
        args_json = payload.get("arguments", "{}")
        return self._send_json(200, {
            "ok": True, "mcp": "echo",
            "target": target_app_id, "tool": tool, "args": args_json
        })

    # ---- GET endpoints ----
    def do_GET(self):
        path = self.path.split("?")[0]
        if path == "/package/status":
            return self._handle_package_status()
        if path == "/health":
            return self._send_json(200, {"ok": True, "service": "nexos-gateway"})
        self._send_json(404, {"error": "not found"})

    def _handle_package_status(self):
        with INSTALLED_LOCK:
            list = []
            for aid, info in INSTALLED.items():
                list.append({"app_id": aid, "size": info["size"],
                             "installed_at": time.strftime("%Y-%m-%dT%H:%M:%S",
                                time.localtime(info["installed_at"]))})
        return self._send_json(200, {"installed": list})


# ---- main ---------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(description="NexOS host-side HTTP gateway")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=19387)
    ap.add_argument("--pkg-dir", default=PKG_DIR)
    args = ap.parse_args()

    set_pkg_dir(args.pkg_dir)
    os.makedirs(PKG_DIR, exist_ok=True)

    server = ThreadingHTTPServer((args.host, args.port), GatewayHandler)
    print("[nexos-gateway] listening on %s:%d" % (args.host, args.port))
    print("[nexos-gateway] pkg dir: %s" % PKG_DIR)
    print("[nexos-gateway] endpoints:")
    print("    POST /package/install   <.nexpack binary>")
    print("    GET  /package/status")
    print("    POST /ai                 {prompt: ...}")
    print("    GET  /health")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\n[nexos-gateway] shutting down")
        server.shutdown()


if __name__ == "__main__":
    main()
