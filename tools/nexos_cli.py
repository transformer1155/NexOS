#!/usr/bin/env python3
import argparse, http.client, json, os, re, shutil, socket, subprocess, sys, zipfile, hashlib

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLS     = os.path.join(REPO_ROOT, "tools")
MEX_PACK  = os.path.join(TOOLS, "mex_pack.py")
NEXPACK   = os.path.join(TOOLS, "nexpack.py")
MEX_PROPS = os.path.join(REPO_ROOT, "csharp", "apps", "mex.shared.props")

APP_ID_RE = re.compile(r"^[a-z][a-z0-9_]{1,31}$")
RESERVED = {"weather", "assistant", "shell", "nexos", "terminal", "calculator",
            "notepad", "browser", "control_panel", "task_manager", "file_explorer",
            "demo", "mem_optimizer", "ai_agent", "ai_setup", "about"}

# 默认 gateway 地址 (宿主侧 Python HTTP server, tools/gateway_server.py)
GATEWAY_HOST = os.environ.get("NEXOS_GATEWAY_HOST", "127.0.0.1")
GATEWAY_PORT = int(os.environ.get("NEXOS_GATEWAY_PORT", "19387"))

def _dotnet_cmd():
    """返回 Windows 下调用 dotnet 的正确命令。
    优先用仓库自带的 tools/dotnet.ps1（已验证能绕过 MSYS2 环境变量剥离问题）。"""
    if sys.platform == "win32":
        ps1 = os.path.join(TOOLS, "dotnet.ps1")
        if os.path.exists(ps1):
            return ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", ps1]
    # 兜底：直接 dotnet
    return ["dotnet"]

def err(m): print("error: " + m, file=sys.stderr)
def ok(m):  print("[ok] " + m)
def info(m): print("     " + m)

def _suggest_nexpack_out(app_dir):
    """Given app dir, return suggested .nexpack path."""
    for f in os.listdir(app_dir):
        if f.endswith(".csproj"):
            return os.path.join(app_dir, f[:-len(".csproj")] + ".nexpack")
    return os.path.join(app_dir, "app.nexpack")


def cmd_new(args):
    name = args.name
    if not APP_ID_RE.match(name): err("bad name: " + name); return 1
    if name in RESERVED: err("reserved: " + name); return 1
    if os.path.exists(name): err("exists: " + name); return 1
    os.makedirs(name)

    program = "using NexOS.Forms;\n\nnamespace App\n{\n    public static class Program\n    {\n        public static void Main()\n        {\n            Shell.Init();\n            Host.OpenApp(Kind.Custom);\n        }\n    }\n}\n"
    with open(os.path.join(name, "Program.cs"), "w", encoding="utf-8") as f: f.write(program)
    ok("Program.cs")

    pfx = MEX_PROPS.replace(os.sep, "/")
    csproj = "<Project Sdk=\"Microsoft.NET.Sdk\">\n  <Import Project=\"%s\" />\n  <PropertyGroup>\n    <AssemblyName>%s</AssemblyName>\n    <RootNamespace>App</RootNamespace>\n  </PropertyGroup>\n  <ItemGroup>\n    <Compile Include=\"Program.cs\" />\n  </ItemGroup>\n</Project>\n" % (pfx, name)
    with open(os.path.join(name, name + ".csproj"), "w", encoding="utf-8") as f: f.write(csproj)
    ok(name + ".csproj")

    manifest = {"schema_version": 1, "app_id": name, "name": "My " + name.capitalize(),
                "version": "1.0.0", "author": "developer", "description": "",
                "entry": {"kind": 99, "desktop": True, "taskbar": True},
                "ai_mode_required": "Fast", "mcp_server": [], "mcp_client_access": []}
    with open(os.path.join(name, "manifest.json"), "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2, ensure_ascii=False); f.write("\n")
    ok("manifest.json")

    with open(os.path.join(name, ".gitignore"), "w") as f: f.write("bin/\nobj/\n*.mex\n*.nexpack\n")
    ok(".gitignore")
    print("\nProject '%s' created at ./%s/" % (name, name))
    return 0

def cmd_deploy(args):
    """发送 .nexpack 到宿主侧 gateway 热部署到 QEMU VM。"""
    pkg = args.pkg
    if not pkg.endswith(".nexpack"): err("not a .nexpack: " + pkg); return 1
    if not os.path.exists(pkg): err("not found: " + pkg); return 1

    host = args.host or GATEWAY_HOST
    port = args.port or GATEWAY_PORT
    url  = "%s:%d" % (host, port)

    # 1. 读取包
    data = open(pkg, "rb").read()
    sha  = hashlib.sha256(data).hexdigest()
    info("pkg: %s (%d bytes, sha256 %s...)" % (pkg, len(data), sha[:16]))

    # 2. 快速健康检查
    info("[1/3] ping gateway %s ..." % url)
    try:
        conn = http.client.HTTPConnection(host, port, timeout=3)
        conn.request("GET", "/health")
        resp = conn.getresponse()
        resp.read()
        if resp.status != 200:
            err("gateway unhealthy (HTTP %d)" % resp.status); return 1
        ok("gateway OK")
    except (ConnectionRefusedError, socket.timeout, OSError) as e:
        err("gateway not reachable on %s: %s" % (url, e))
        err("  -> start:  python tools/gateway_server.py"); return 2
    finally:
        try: conn.close()
        except: pass

    # 3. POST .nexpack 二进制
    info("[2/3] POST /package/install (%d bytes) ..." % len(data))
    try:
        conn = http.client.HTTPConnection(host, port, timeout=10)
        conn.request("POST", "/package/install", data, {
            "Content-Type": "application/octet-stream",
            "Content-Length": str(len(data)),
            "X-Nexos-SHA256": sha,
        })
        resp = conn.getresponse()
        body = resp.read().decode("utf-8", errors="replace")
    except Exception as e:
        err("send failed: " + str(e)); return 1
    finally:
        try: conn.close()
        except: pass

    # 4. 解析响应
    try: payload = json.loads(body)
    except: payload = {"raw": body}

    if resp.status == 200 and payload.get("ok"):
        report = payload.get("report", {})
        ok("deployed!")
        print("     app_id : " + report.get("app_id", "?"))
        print("     version: " + report.get("version", "?"))
        print("     mex    : %d bytes" % report.get("mex_size", 0))
        print("     sha256 : " + report.get("sha256", "?"))
    else:
        err("gateway HTTP %d: %s" % (resp.status, payload.get("error", body[:200])))
        return 1
    return 0



def cmd_build(args):
    cwd = os.getcwd()
    csprojs = [f for f in os.listdir(cwd) if f.endswith(".csproj")]
    if not csprojs: err("no .csproj"); return 1
    if len(csprojs) > 1: err("multiple .csproj"); return 1
    csproj = csprojs[0]
    app_name = csproj[:-len(".csproj")]
    info("csproj: " + csproj + "  app: " + app_name)

    # dotnet build — 必须用完整 Windows 用户环境
    info("[1/3] dotnet build (Release)...")
    rc = run(["dotnet", "build", csproj, "-c", "Release", "-v", "minimal"], dotnet=True)
    if rc != 0: err("dotnet build failed"); return 1
    ok("dotnet build OK")

    dll_path = find_dll(cwd, app_name)
    if not dll_path: err("cannot find " + app_name + ".dll"); return 1
    info("dll: " + dll_path)

    mex_path = os.path.join(cwd, app_name + ".mex")
    info("[2/3] mex_pack -> " + app_name + ".mex")
    rc = run(["python", MEX_PACK, dll_path, mex_path])
    if rc != 0: err("mex_pack failed"); return 1
    ok(app_name + ".mex OK (%d bytes)" % os.path.getsize(mex_path))

    manifest_path = os.path.join(cwd, "manifest.json")
    if not os.path.exists(manifest_path): err("manifest.json missing"); return 1
    out_path = args.out or os.path.join(cwd, app_name + ".nexpack")
    info("[3/3] nexpack -> " + os.path.basename(out_path))
    cmd_pack = ["python", NEXPACK, "pack", "--mex", mex_path, "--manifest", manifest_path]
    icon_path = os.path.join(cwd, "icon.png")
    if os.path.exists(icon_path): cmd_pack += ["--icon", icon_path]
    cmd_pack += ["--out", out_path]
    rc = run(cmd_pack)
    if rc != 0: err("nexpack failed"); return 1
    ok(app_name + ".nexpack OK (%d bytes)" % os.path.getsize(out_path))
    print("\nnexpack written: " + out_path)
    return 0

def find_dll(cwd, app_name):
    for sub in ["bin/Release/net8.0/" + app_name + ".dll", "bin/Release/netstandard2.0/" + app_name + ".dll"]:
        p = os.path.join(cwd, sub)
        if os.path.exists(p): return p
    for root, dirs, files in os.walk(os.path.join(cwd, "bin")):
        for f in files:
            if f.lower() == (app_name + ".dll").lower(): return os.path.join(root, f)
    return None

def cmd_verify(args):
    pkg = args.pkg
    if not os.path.exists(pkg): err("not found: " + pkg); return 1
    errors = []
    try: z = zipfile.ZipFile(pkg, "r")
    except zipfile.BadZipFile as e: err("bad zip: " + str(e)); return 1
    for info in z.infolist():
        if info.compress_type != zipfile.ZIP_STORED:
            errors.append("compressed (must be STORE): " + info.filename)
    if "manifest.json" not in z.namelist(): errors.append("missing manifest.json")
    else:
        try: errors.extend(validate_manifest_local(json.loads(z.read("manifest.json"))))
        except Exception as e: errors.append("manifest parse: " + str(e))
    if "app.mex" not in z.namelist(): errors.append("missing app.mex")
    else:
        mex = z.read("app.mex")
        if len(mex) < 4 or mex[:4] != b"MEX1": errors.append("bad app.mex magic (want MEX1)")
        if len(mex) > 2 * 1024 * 1024: errors.append("app.mex > 2MB")
    total = os.path.getsize(pkg)
    if total > 4 * 1024 * 1024: errors.append("pkg > 4MB")
    if errors:
        print("FAIL: %d issues" % len(errors))
        for e in errors: print("  - " + e)
        return 1
    sha = hashlib.sha256(open(pkg, "rb").read()).hexdigest()
    print("OK: %s (%d bytes, sha256 %s)" % (pkg, total, sha))
    return 0

def validate_manifest_local(m):
    e = []
    if not isinstance(m, dict): return ["top-level not object"]
    if m.get("schema_version") != 1: e.append("schema_version must be 1")
    aid = m.get("app_id")
    if not aid: e.append("app_id missing")
    elif not APP_ID_RE.match(aid): e.append("app_id bad format: " + aid)
    elif aid in RESERVED: e.append("app_id reserved")
    for k in ("name", "version", "author"):
        if not m.get(k): e.append(k + " missing")
    return e

def cmd_unpack(args):
    pkg = args.pkg
    out = args.out or (pkg + ".extracted")
    os.makedirs(out, exist_ok=True)
    with zipfile.ZipFile(pkg, "r") as z: z.extractall(out)
    print("extracted -> " + out)
    return 0

def run(cmd, dotnet=False):
    """Run a subprocess, streaming output directly to parent (no deadlock)."""
    if dotnet:
        # Windows 下用 PowerShell 调 dotnet —— pwsh 自带完整用户环境
        # (USERPROFILE/APPDATA/HOMEDRIVE 等)，这是 NuGet restore 必需的
        args_str = " ".join(cmd[1:])   # 去掉 "dotnet"，保留其余 args
        cmd = ["powershell", "-NoProfile", "-Command",
               r"& 'C:\Program Files\dotnet\dotnet.exe' " + args_str]
    info("$ " + " ".join(cmd))
    rc = subprocess.call(cmd)
    if rc != 0: print("     ! exit code: %d" % rc)
    return rc

def main():
    ap = argparse.ArgumentParser(prog="nexos", description="NexOS dev CLI")
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("new", help="create project"); p.add_argument("name")
    p = sub.add_parser("build", help="one-shot build"); p.add_argument("--out", "-o")
    p = sub.add_parser("verify", help="verify .nexpack"); p.add_argument("pkg")
    p = sub.add_parser("unpack", help="unpack .nexpack"); p.add_argument("pkg"); p.add_argument("--out", "-o")
    p = sub.add_parser("deploy", help="deploy .nexpack to VM"); p.add_argument("pkg"); p.add_argument("--host"); p.add_argument("--port", type=int)
    args = ap.parse_args()
    if args.cmd == "build":
        if not os.path.exists(MEX_PACK): err("mex_pack.py missing"); return 2
        if not os.path.exists(NEXPACK): err("nexpack.py missing"); return 2
        if shutil.which("dotnet") is None: err("dotnet not in PATH"); return 2
    return {"new": cmd_new, "build": cmd_build, "verify": cmd_verify, "unpack": cmd_unpack, "deploy": cmd_deploy}[args.cmd](args)

if __name__ == "__main__": sys.exit(main())
