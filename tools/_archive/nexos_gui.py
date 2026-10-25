#!/usr/bin/env python3
"""NexOS Developer Toolkit GUI (WebView2 + pywebview)."""
import hashlib, http.client, json, os, re, shutil, socket, subprocess, sys, threading, time, zipfile

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLS     = os.path.join(REPO_ROOT, "tools")
MEX_PACK  = os.path.join(TOOLS, "mex_pack.py")
NEXPACK   = os.path.join(TOOLS, "nexpack.py")
MEX_PROPS = os.path.join(REPO_ROOT, "csharp", "apps", "mex.shared.props")

APP_ID_RE = re.compile(r"^[a-z][a-z0-9_]{1,31}$")
RESERVED  = {"weather","assistant","shell","nexos","terminal","calculator",
             "notepad","browser","control_panel","task_manager","file_explorer",
             "demo","mem_optimizer","ai_agent","ai_setup","about"}

GW_HOST = "127.0.0.1"
GW_PORT = 19387

gw_proc = None
gw_proc_lock = threading.Lock()
_jobs = {}

def _py(cmd, cwd=None, timeout=300):
    try:
        p = subprocess.run(["python"] + cmd, cwd=cwd, timeout=timeout,
                          capture_output=True, text=True, encoding="utf-8", errors="replace")
        return p.returncode, p.stdout, p.stderr
    except subprocess.TimeoutExpired: return -1, "", "timeout"
    except Exception as e: return -2, "", str(e)

def _dotnet(args, cwd=None, timeout=300):
    ps = ["powershell", "-NoProfile", "-Command",
          r"& 'C:\Program Files\dotnet\dotnet.exe' " + " ".join(args)]
    try:
        p = subprocess.run(ps, cwd=cwd, timeout=timeout,
                          capture_output=True, text=True, encoding="utf-8", errors="replace")
        return p.returncode, p.stdout, p.stderr
    except Exception as e: return -2, "", str(e)

def _hash(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""): h.update(chunk)
    return h.hexdigest()


class Bridge:
    def new_project(self, name, base_dir):
        if not APP_ID_RE.match(name):
            return {"ok": False, "message": "bad app_id (must match ^[a-z][a-z0-9_]{1,31}$)"}
        if name in RESERVED:
            return {"ok": False, "message": "reserved app_id"}
        target = os.path.join(base_dir, name)
        if os.path.exists(target):
            return {"ok": False, "message": "already exists"}
        os.makedirs(target)
        prog = "using NexOS.Forms;\n\nnamespace App\n{\n    public static class Program\n    {\n        public static void Main()\n        { Shell.Init(); Host.OpenApp(Kind.Custom); }\n    }\n}\n"
        open(os.path.join(target, "Program.cs"), "w", encoding="utf-8").write(prog)
        props = MEX_PROPS.replace(os.sep, "/")
        csproj = '<Project Sdk="Microsoft.NET.Sdk">\n  <Import Project="%s" />\n  <PropertyGroup><AssemblyName>%s</AssemblyName><RootNamespace>App</RootNamespace></PropertyGroup>\n  <ItemGroup><Compile Include="Program.cs" /></ItemGroup>\n</Project>\n' % (props, name)
        open(os.path.join(target, name + ".csproj"), "w", encoding="utf-8").write(csproj)
        manifest = {"schema_version": 1, "app_id": name, "name": name.capitalize(),
                    "version": "1.0.0", "author": "developer", "description": "",
                    "entry": {"kind": 99, "desktop": True, "taskbar": True},
                    "ai_mode_required": "Fast", "mcp_server": [], "mcp_client_access": []}
        open(os.path.join(target, "manifest.json"), "w", encoding="utf-8").write(
            json.dumps(manifest, indent=2, ensure_ascii=False))
        open(os.path.join(target, ".gitignore"), "w").write("bin/\nobj/\n*.mex\n*.nexpack\n")
        return {"ok": True, "message": "created", "path": target}

    def build_start(self, csproj_dir):
        job_id = f"j{int(time.time()*1000)}"
        def _run():
            log = []
            def L(s): log.append(s)
            try:
                csprojs = [f for f in os.listdir(csproj_dir) if f.endswith(".csproj")]
                if len(csprojs) != 1:
                    _jobs[job_id] = {"status": "done", "ok": False, "log": log,
                                     "error": f"need 1 .csproj, got {len(csprojs)}"}; return
                app = csprojs[0][:-8]
                L(f"[{app}] dotnet build Release...")
                rc, out, err = _dotnet(["build", csprojs[0], "-c", "Release", "-v", "minimal"], cwd=csproj_dir)
                L(out + err)
                if rc != 0:
                    _jobs[job_id] = {"status": "done", "ok": False, "log": log, "error": "dotnet build rc=%d" % rc}; return
                dll = None
                for root, _, files in os.walk(csproj_dir):
                    for f in files:
                        if f.endswith(".dll") and root.find("obj") < 0:
                            dll = os.path.join(root, f); break
                    if dll: break
                if not dll:
                    _jobs[job_id] = {"status": "done", "ok": False, "log": log, "error": "no .dll found"}; return
                mex = os.path.join(csproj_dir, app + ".mex")
                L(f"[{app}] mex_pack {dll} -> {mex}")
                rc2, out2, err2 = _py([MEX_PACK, dll, mex])
                L(out2 + err2)
                if rc2 != 0:
                    _jobs[job_id] = {"status": "done", "ok": False, "log": log, "error": "mex_pack rc=%d" % rc2}; return
                nexpack = os.path.join(csproj_dir, app + ".nexpack")
                manifest = os.path.join(csproj_dir, "manifest.json")
                L(f"[{app}] nexpack pack -> {nexpack}")
                rc3, out3, err3 = _py([NEXPACK, "pack", "--mex", mex, "--manifest", manifest, "--out", nexpack])
                L(out3 + err3)
                if rc3 != 0:
                    _jobs[job_id] = {"status": "done", "ok": False, "log": log, "error": "nexpack rc=%d" % rc3}; return
                sha = _hash(nexpack)
                _jobs[job_id] = {"status": "done", "ok": True, "log": log,
                                 "nexpack": nexpack, "sha256": sha,
                                 "size": os.path.getsize(nexpack)}
            except Exception as e:
                _jobs[job_id] = {"status": "done", "ok": False, "log": log, "error": str(e)}
        _jobs[job_id] = {"status": "running", "log": []}
        threading.Thread(target=_run, daemon=True).start()
        return {"job_id": job_id}

    def get_job(self, job_id):
        return _jobs.get(job_id, {"status": "unknown"})

    def verify(self, pkg_path):
        if not os.path.exists(pkg_path): return {"ok": False, "error": "not found"}
        rc, out, err = _py([NEXPACK, "verify", pkg_path])
        if rc == 0: return {"ok": True, "message": out.strip()}
        return {"ok": False, "message": (out + err).strip()}

    def deploy(self, pkg_path, host=None, port=None):
        h = host or GW_HOST; p = port or GW_PORT
        if not os.path.exists(pkg_path): return {"ok": False, "error": "not found"}
        data = open(pkg_path, "rb").read()
        sha = hashlib.sha256(data).hexdigest()
        try:
            c = http.client.HTTPConnection(h, p, timeout=3)
            c.request("GET", "/health"); resp = c.getresponse(); resp.read(); c.close()
            if resp.status != 200: return {"ok": False, "error": "gateway HTTP %d" % resp.status}
        except Exception as e:
            return {"ok": False, "error": f"gateway unreachable: {e}", "hint": "start: python tools/net/gateway_server.py"}
        try:
            c = http.client.HTTPConnection(h, p, timeout=10)
            c.request("POST", "/package/install", data, {
                "Content-Type": "application/octet-stream", "X-Nexos-SHA256": sha})
            resp = c.getresponse()
            body = resp.read().decode("utf-8", errors="replace"); c.close()
            payload = json.loads(body) if body else {}
            if resp.status == 200 and payload.get("ok"):
                return {"ok": True, "report": payload.get("report", {})}
            return {"ok": False, "http": resp.status, "error": payload.get("error", body[:200])}
        except Exception as e:
            return {"ok": False, "error": str(e)}

    def gw_status(self, host=None, port=None):
        h = host or GW_HOST; p = port or GW_PORT
        try:
            c = http.client.HTTPConnection(h, p, timeout=2)
            c.request("GET", "/health"); r = c.getresponse(); r.read(); c.close()
            return {"running": r.status == 200, "host": h, "port": p}
        except: return {"running": False, "host": h, "port": p}

    def gw_start(self, port=None, pkg_dir=None):
        global gw_proc
        with gw_proc_lock:
            if gw_proc and gw_proc.poll() is None:
                return {"ok": False, "message": "already running"}
            args = [os.path.join(TOOLS, "gateway_server.py"), "--port", str(port or GW_PORT)]
            if pkg_dir: args += ["--pkg-dir", pkg_dir]
            gw_proc = subprocess.Popen(["python"] + args,
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            time.sleep(0.8)
            return {"ok": gw_proc.poll() is None, "pid": gw_proc.pid}

    def gw_stop(self):
        global gw_proc
        with gw_proc_lock:
            if gw_proc and gw_proc.poll() is None:
                gw_proc.terminate()
                try: gw_proc.wait(timeout=3)
                except: gw_proc.kill()
            gw_proc = None
            return {"ok": True}

    def pick_file(self, pattern="*.nexpack"):
        try:
            import tkinter as tk
            from tkinter import filedialog
            tk.Tk().withdraw()
            return filedialog.askopenfilename(title="Select .nexpack",
                                              filetypes=[("Nexpack", "*.nexpack"), ("All", "*.*")])
        except: return None

    def pick_dir(self):
        try:
            import tkinter as tk
            from tkinter import filedialog
            tk.Tk().withdraw()
            return filedialog.askdirectory(title="Select directory")
        except: return None

    def list_projects(self, base_dir):
        try:
            dirs = []
            for d in os.listdir(base_dir):
                full = os.path.join(base_dir, d)
                if os.path.isdir(full) and any(f.endswith(".csproj") for f in os.listdir(full)):
                    dirs.append(d)
            return {"ok": True, "projects": sorted(dirs)}
        except Exception as e: return {"ok": False, "error": str(e)}


FRONTEND_HTML = """<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<title>NexOS Developer Toolkit</title>
<script src="https://cdn.tailwindcss.com"></script>
<style>
  body { font-family: "Segoe UI", system-ui, sans-serif; background: #0f172a; color: #e2e8f0; }
  .tab-active { background: #1e293b; border-bottom: 2px solid #38bdf8; color: #38bdf8; }
  .tab { padding: 12px 20px; cursor: pointer; color: #94a3b8; transition: all .15s; }
  .tab:hover { color: #e2e8f0; background: #1e293b; }
  .panel { display: none; }
  .panel.active { display: block; }
  .log { background: #020617; color: #a3e635; font-family: Consolas, monospace; font-size: 12px; padding: 12px; border-radius: 8px; height: 300px; overflow-y: auto; white-space: pre-wrap; }
  .btn { padding: 8px 16px; border-radius: 6px; font-weight: 500; }
  .btn-primary { background: #0284c7; color: white; }
  .btn-primary:hover { background: #0369a1; }
  .btn-primary:disabled { background: #475569; cursor: not-allowed; }
  .btn-success { background: #16a34a; color: white; }
  .btn-success:hover { background: #15803d; }
  .btn-danger { background: #dc2626; color: white; }
  .btn-danger:hover { background: #b91c1c; }
  .input { background: #1e293b; border: 1px solid #334155; border-radius: 6px; padding: 8px 12px; color: #e2e8f0; width: 100%; }
  .input:focus { outline: none; border-color: #38bdf8; }
  .card { background: #1e293b; border-radius: 12px; padding: 20px; border: 1px solid #334155; }
  .ok-badge { background: #14532d; color: #86efac; padding: 4px 10px; border-radius: 12px; font-size: 12px; }
  .err-badge { background: #7f1d1d; color: #fca5a5; padding: 4px 10px; border-radius: 12px; font-size: 12px; }
  .warn-badge { background: #713f12; color: #fde047; padding: 4px 10px; border-radius: 12px; font-size: 12px; }
</style>
</head>
<body class="min-h-screen">
<header class="border-b border-slate-700 px-6 py-4 flex items-center justify-between">
  <div class="flex items-center gap-3">
    <div class="w-10 h-10 bg-gradient-to-br from-cyan-400 to-blue-600 rounded-lg flex items-center justify-center font-bold text-lg">N</div>
    <div><h1 class="text-xl font-bold">NexOS Developer Toolkit</h1><p class="text-xs text-slate-400">WebView2</p></div>
  </div>
  <span id="gw-indicator" class="warn-badge">checking gateway...</span>
</header>
<nav class="flex border-b border-slate-700 bg-slate-900">
  <div class="tab tab-active" data-tab="new">New Project</div>
  <div class="tab" data-tab="build">Build</div>
  <div class="tab" data-tab="verify">Verify</div>
  <div class="tab" data-tab="deploy">Deploy</div>
  <div class="tab" data-tab="gateway">Gateway</div>
</nav>
<main class="p-6 max-w-5xl mx-auto">

<section id="panel-new" class="panel active">
<div class="card"><h2 class="text-lg font-semibold mb-4">Create MiniCLR Project</h2>
<div class="space-y-4">
  <div><label class="text-sm text-slate-400">App ID</label><input id="new-name" class="input mt-1" placeholder="e.g. my_notepad"></div>
  <div><label class="text-sm text-slate-400">Base Directory</label>
    <div class="flex gap-2 mt-1"><input id="new-dir" class="input flex-1"><button class="btn bg-slate-700" onclick="pickNewDir()">...</button></div></div>
  <button class="btn btn-primary" onclick="doNew()">Create Project</button>
  <div id="new-result"></div>
</div></div></section>

<section id="panel-build" class="panel">
<div class="card"><h2 class="text-lg font-semibold mb-4">Build & Pack</h2>
<div class="space-y-4">
  <div><label class="text-sm text-slate-400">Project Directory</label>
    <div class="flex gap-2 mt-1"><input id="build-dir" class="input flex-1"><button class="btn bg-slate-700" onclick="pickBuildDir()">...</button></div></div>
  <div id="build-projects" class="text-sm text-slate-400"></div>
  <button id="build-btn" class="btn btn-primary" onclick="doBuild()">Build (dotnet → mex → nexpack)</button>
  <div id="build-log" class="log"></div>
</div></div></section>

<section id="panel-verify" class="panel">
<div class="card"><h2 class="text-lg font-semibold mb-4">Verify .nexpack</h2>
<div class="space-y-4">
  <div class="flex gap-2"><input id="verify-path" class="input flex-1"><button class="btn bg-slate-700" onclick="pickVerifyFile()">...</button></div>
  <button class="btn btn-primary" onclick="doVerify()">Verify</button>
  <div id="verify-result"></div>
</div></div></section>

<section id="panel-deploy" class="panel">
<div class="card"><h2 class="text-lg font-semibold mb-4">Deploy to VM Gateway</h2>
<div class="space-y-4">
  <div class="flex gap-2"><input id="deploy-path" class="input flex-1" placeholder="path to .nexpack"><button class="btn bg-slate-700" onclick="pickDeployFile()">...</button></div>
  <div class="grid grid-cols-2 gap-4">
    <div><label class="text-sm text-slate-400">Host</label><input id="deploy-host" class="input mt-1" value="127.0.0.1"></div>
    <div><label class="text-sm text-slate-400">Port</label><input id="deploy-port" class="input mt-1" value="19387" type="number"></div>
  </div>
  <button class="btn btn-success" onclick="doDeploy()">Deploy</button>
  <div id="deploy-result"></div>
</div></div></section>

<section id="panel-gateway" class="panel">
<div class="card"><h2 class="text-lg font-semibold mb-4">Gateway Manager</h2>
<div class="space-y-4">
  <div class="p-4 rounded-lg bg-slate-800 flex items-center justify-between">
    <div><div class="text-sm text-slate-400">Status</div><div id="gw-status-text" class="text-lg">checking...</div></div>
    <div id="gw-actions" class="flex gap-2"></div>
  </div>
  <div class="grid grid-cols-2 gap-4">
    <div><label class="text-sm text-slate-400">Port</label><input id="gw-port" class="input mt-1" value="19387" type="number"></div>
    <div><label class="text-sm text-slate-400">Package Dir</label><input id="gw-pkgdir" class="input mt-1"></div>
  </div>
</div></div></section>

</main>
<script>
document.querySelectorAll('.tab').forEach(t => t.addEventListener('click', () => {
  document.querySelectorAll('.tab').forEach(x => x.classList.remove('tab-active'));
  document.querySelectorAll('.panel').forEach(x => x.classList.remove('active'));
  t.classList.add('tab-active');
  document.getElementById('panel-' + t.dataset.tab).classList.add('active');
}));
function badge(type, text) {
  const cls = { ok: 'ok-badge', err: 'err-badge', warn: 'warn-badge' }[type];
  return '<span class="' + cls + '">' + text + '</span>';
}
async function bridge(method, args) {
  if (!args) args = [];
  return await window.pywebview.api[method](...args);
}
async function doNew() {
  const name = document.getElementById('new-name').value.trim();
  const dir  = document.getElementById('new-dir').value.trim();
  const r = await bridge('new_project', [name, dir]);
  document.getElementById('new-result').innerHTML = r.ok
    ? '<div class="mt-3 p-3 rounded-lg bg-slate-800">' + badge('ok', 'OK') + ' Created: <code>' + r.path + '</code></div>'
    : '<div class="mt-3 p-3 rounded-lg bg-red-900/40">' + badge('err', 'FAIL') + ' ' + r.message + '</div>';
}
async function pickNewDir() { const d = await bridge('pick_dir', []); if (d) document.getElementById('new-dir').value = d; }
async function pickBuildDir() { const d = await bridge('pick_dir', []); if (d) { document.getElementById('build-dir').value = d; listProjs(d); } }
async function listProjs(d) {
  const r = await bridge('list_projects', [d]);
  document.getElementById('build-projects').textContent = r.ok ? 'Found: ' + r.projects.join(', ') : r.error;
}
let buildPoll = null;
async function doBuild() {
  const dir = document.getElementById('build-dir').value.trim();
  const btn = document.getElementById('build-btn');
  const log = document.getElementById('build-log');
  log.textContent = ''; btn.disabled = true;
  const r = await bridge('build_start', [dir]);
  buildPoll = setInterval(async () => {
    const st = await bridge('get_job', [r.job_id]);
    log.textContent = (st.log || []).join('\n');
    log.scrollTop = log.scrollHeight;
    if (st.status === 'done') {
      clearInterval(buildPoll); btn.disabled = false;
      log.textContent += '\n\n' + (st.ok ? 'SUCCESS: ' + st.nexpack + ' (' + st.size + ' bytes)\nSHA256: ' + st.sha256 : '\n\nFAIL: ' + st.error);
    }
  }, 500);
}
async function pickVerifyFile() { const f = await bridge('pick_file', ['*.nexpack']); if (f) document.getElementById('verify-path').value = f; }
async function doVerify() {
  const p = document.getElementById('verify-path').value.trim();
  const r = await bridge('verify', [p]);
  document.getElementById('verify-result').innerHTML = r.ok
    ? '<div class="mt-3 p-3 rounded-lg bg-slate-800">' + badge('ok', 'VALID') + '<br><span class="text-xs text-slate-400">' + r.message + '</span></div>'
    : '<div class="mt-3 p-3 rounded-lg bg-red-900/40">' + badge('err', 'INVALID') + ' ' + r.message + '</div>';
}
async function pickDeployFile() { const f = await bridge('pick_file', ['*.nexpack']); if (f) document.getElementById('deploy-path').value = f; }
async function doDeploy() {
  const p = document.getElementById('deploy-path').value.trim();
  const h = document.getElementById('deploy-host').value.trim();
  const port = parseInt(document.getElementById('deploy-port').value.trim());
  const el = document.getElementById('deploy-result');
  el.innerHTML = '<div class="p-3 text-slate-400">deploying...</div>';
  const r = await bridge('deploy', [p, h, port]);
  if (r.ok) {
    const rep = r.report;
    el.innerHTML = '<div class="mt-3 p-3 rounded-lg bg-slate-800">' + badge('ok', 'DEPLOYED')
      + '<div class="text-xs text-slate-400 mt-2">app_id: <code>' + rep.app_id + '</code> | version: ' + rep.version + ' | mex: ' + rep.mex_size + ' bytes | sha256: <code>' + rep.sha256.substring(0,32) + '...</code></div></div>';
  } else {
    el.innerHTML = '<div class="mt-3 p-3 rounded-lg bg-red-900/40">' + badge('err', 'FAIL') + ' ' + (r.error || r.http) + '</div>';
  }
}
async function refreshGwStatus() {
  const st = await bridge('gw_status', []);
  document.getElementById('gw-indicator').textContent = st.running ? 'RUNNING ' + st.host + ':' + st.port : 'STOPPED';
  document.getElementById('gw-indicator').className = st.running ? 'ok-badge' : 'err-badge';
  document.getElementById('gw-status-text').textContent = st.running ? 'Listening on ' + st.host + ':' + st.port : 'Not running';
  const actions = document.getElementById('gw-actions');
  if (st.running) {
    actions.innerHTML = '<button class="btn btn-danger" onclick="doGwStop()">Stop</button><button class="btn bg-slate-700" onclick="refreshGwStatus()">Refresh</button>';
  } else {
    actions.innerHTML = '<button class="btn btn-success" onclick="doGwStart()">Start</button><button class="btn bg-slate-700" onclick="refreshGwStatus()">Refresh</button>';
  }
}
async function doGwStart() {
  const port = parseInt(document.getElementById('gw-port').value);
  const pkgDir = document.getElementById('gw-pkgdir').value.trim() || null;
  await bridge('gw_start', [port, pkgDir]);
  setTimeout(refreshGwStatus, 1500);
}
async function doGwStop() { await bridge('gw_stop', []); setTimeout(refreshGwStatus, 500); }

window.addEventListener('pywebviewready', async () => {
  refreshGwStatus();
});
</script>
</body>
</html>
"""


def main():
    try:
        import webview
    except ImportError:
        print("error: pywebview missing. run: pip install pywebview", file=sys.stderr)
        sys.exit(1)
    html_path = os.path.join(TOOLS, "_gui.html")
    open(html_path, "w", encoding="utf-8").write(FRONTEND_HTML)
    bridge = Bridge()
    webview.create_window(
        "NexOS Developer Toolkit",
        url="file:///" + html_path.replace("\\", "/"),
        width=1100, height=720, min_size=(900, 560),
        resizable=True, text_select=True,
        js_api=bridge,
    )
    webview.start(debug=False)

if __name__ == "__main__":
    main()
