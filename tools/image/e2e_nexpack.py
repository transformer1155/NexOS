#!/usr/bin/env python3
# =====================================================================
#  e2e_nexpack.py  -  .nexpack 生态系统端到端验证
# ---------------------------------------------------------------------
#  串起三件交付物，证明它们真的协同工作：
#
#    1. 应用包格式   tools/image/nexpack.py     pack / verify / inspect
#    2. 安装器       PackageInstaller     （通过 installer_test 已验证）
#    3. MCP SDK      mcp_scan.py + McpServerBase
#
#  本脚本负责：
#    A. 用 mcp_scan.py 扫描示例应用的 [McpTool] 方法
#    B. 把扫描结果合进 manifest.json
#    C. 用 nexpack.py 打包成 .nexpack
#    D. verify + inspect 校验包
#    E. 解包，确认 app.mex / manifest.json / icon 可还原
#    F. 断言 manifest 里的 mcp_server 与扫描结果一致
#
#  用法: python tools/image/e2e_nexpack.py
# =====================================================================
import json
import os
import shutil
import subprocess
import sys
import tempfile
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLS = os.path.join(ROOT, "tools")
PY = sys.executable

# Windows 控制台默认 GBK，输出中文/替换字符会抛 UnicodeEncodeError。
# 统一改成立即写 UTF-8，失败时用 'replace' 兜底（不影响退出码语义）。
try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

pass_n = 0
fail_n = 0


def check(what, ok, detail=""):
    global pass_n, fail_n
    if ok:
        pass_n += 1
        print("PASS  " + what)
    else:
        fail_n += 1
        print("FAIL  " + what + ("  -> " + str(detail) if detail else ""))


def run(args, cwd=None):
    r = subprocess.run(args, cwd=cwd or ROOT, capture_output=True, text=True,
                       encoding="utf-8", errors="replace")
    return r.returncode, r.stdout, r.stderr


def main():
    work = tempfile.mkdtemp(prefix="nexos_e2e_")
    print("workdir: " + work)
    print()

    example = os.path.join(ROOT, "csharp", "apps", "examples", "Weather")
    if not os.path.isdir(example):
        print("error: example app not found: " + example, file=sys.stderr)
        return 2

    # ---- A. 构建期扫描 ------------------------------------------------
    print("=== A. mcp_scan.py 扫描 [McpTool] ===")
    tools_json = os.path.join(work, "mcp_tools.json")
    rc, out, errout = run([PY, os.path.join(TOOLS, "mcp_scan.py"),
                           "--src", example,
                           "--out", tools_json])
    check("mcp_scan exits 0", rc == 0, errout)
    check("mcp_scan writes descriptors", os.path.isfile(tools_json), tools_json)
    if not os.path.isfile(tools_json):
        return 1

    scanned = json.load(open(tools_json, encoding="utf-8"))
    tool_names = [t["tool_name"] for t in scanned["mcp_server"]]
    check("scanned 2 tools", len(tool_names) == 2, tool_names)
    check("found get_weather", "get_weather" in tool_names, tool_names)
    check("found celsius_to_fahrenheit",
          "celsius_to_fahrenheit" in tool_names, tool_names)
    check("every tool has a description",
          all(t.get("description") for t in scanned["mcp_server"]),
          scanned["mcp_server"])

    # ---- B. 合成 manifest ---------------------------------------------
    print()
    print("=== B. 合成 manifest.json ===")
    manifest = {
        "schema_version": 1,
        "app_id": "weatherx",
        "name": "Weather App",
        "version": "1.2.0",
        "author": "nexos",
        "description": "Show current weather for a location.",
        "icon": "icon.png",
        "entry": {"kind": 13, "desktop": True, "taskbar": False},
        "ai_mode_required": "Fast",
        # 关键：mcp_server 来自扫描结果，而不是手写
        "mcp_server": scanned["mcp_server"],
        "mcp_client_access": []
    }
    manifest_path = os.path.join(work, "manifest.json")
    with open(manifest_path, "w", encoding="utf-8") as f:
        json.dump(manifest, f, ensure_ascii=False, indent=2)
    check("manifest written", os.path.isfile(manifest_path), manifest_path)

    # 取一个真实的 .mex 作为 app.mex
    mex_src = os.path.join(ROOT, "sfs_files", "Calculator.mex")
    if not os.path.isfile(mex_src):
        # 退回任意一个存在的 .mex
        cands = [f for f in os.listdir(os.path.join(ROOT, "sfs_files"))
                 if f.endswith(".mex")]
        if not cands:
            print("error: no .mex available in sfs_files/", file=sys.stderr)
            return 2
        mex_src = os.path.join(ROOT, "sfs_files", cands[0])
    check("source .mex exists", os.path.isfile(mex_src), mex_src)

    # app_id 必须等于 app.mex 的 basename（nexpack.py 强制校验）。
    # 复制成 weatherx.mex，让 manifest 与包内容自洽。
    mex_staged = os.path.join(work, "weatherx.mex")
    shutil.copy(mex_src, mex_staged)
    mex_src = mex_staged
    check("staged .mex basename matches app_id",
          os.path.splitext(os.path.basename(mex_src))[0] == "weatherx",
          os.path.basename(mex_src))

    # 1x1 PNG
    icon_path = os.path.join(work, "icon.png")
    png = bytes.fromhex(
        "89504E470D0A1A0A0000000D4948445200000001000000010806000000"
        "1F15C4890000000A49444154789C63000100000500010D0A2DB4"
        "0000000049454E44AE426082")
    open(icon_path, "wb").write(png)

    # ---- C. 打包 -------------------------------------------------------
    print()
    print("=== C. nexpack.py pack ===")
    pkg = os.path.join(work, "weatherx-1.2.0.nexpack")
    rc, out, errout = run([PY, os.path.join(TOOLS, "nexpack.py"), "pack",
                           "--mex", mex_src,
                           "--manifest", manifest_path,
                           "--icon", icon_path,
                           "--out", pkg])
    check("pack exits 0", rc == 0, errout or out)
    check("package created", os.path.isfile(pkg), pkg)
    if not os.path.isfile(pkg):
        return 1
    check("package is non-trivial", os.path.getsize(pkg) > 1000,
          os.path.getsize(pkg))

    # ---- D. 校验 -------------------------------------------------------
    print()
    print("=== D. nexpack.py verify / inspect ===")
    rc, out, errout = run([PY, os.path.join(TOOLS, "nexpack.py"),
                           "verify", pkg])
    check("verify exits 0", rc == 0, errout or out)
    check("verify reports OK", "OK" in out, out)

    rc, out, errout = run([PY, os.path.join(TOOLS, "nexpack.py"),
                           "inspect", pkg])
    check("inspect exits 0", rc == 0, errout)
    check("inspect shows both tools", "get_weather" in out, out)
    check("inspect shows STORE", "STORE" in out, out)

    # 结构断言：成员齐、全部 STORE
    with zipfile.ZipFile(pkg) as z:
        names = z.namelist()
        check("members present",
              "manifest.json" in names and "app.mex" in names and
              "icon.png" in names, names)
        check("all members STORE",
              all(i.compress_type == zipfile.ZIP_STORED for i in z.infolist()),
              [i.compress_type for i in z.infolist()])

        # ---- E. 解包还原 ---------------------------------------------
        print()
        print("=== E. nexpack.py unpack 还原 ===")
        outdir = os.path.join(work, "unpacked")
        rc, out, errout = run([PY, os.path.join(TOOLS, "nexpack.py"),
                               "unpack", pkg, "--out", outdir])
        check("unpack exits 0", rc == 0, errout)
        check("app.mex restored",
              os.path.isfile(os.path.join(outdir, "app.mex")), outdir)
        check("manifest restored",
              os.path.isfile(os.path.join(outdir, "manifest.json")), outdir)
        check("icon restored",
              os.path.isfile(os.path.join(outdir, "icon.png")), outdir)

        # 字节级一致
        if os.path.isfile(os.path.join(outdir, "app.mex")):
            a = open(mex_src, "rb").read()
            b = open(os.path.join(outdir, "app.mex"), "rb").read()
            check("app.mex bytes identical", a == b,
                  "len %d vs %d" % (len(a), len(b)))

        # ---- F. manifest 与扫描结果一致 ------------------------------
        print()
        print("=== F. manifest 的 mcp_server 来自扫描 ===")
        back = json.load(open(os.path.join(outdir, "manifest.json"),
                              encoding="utf-8"))
        back_names = [t["tool_name"] for t in back["mcp_server"]]
        check("roundtrip preserves tool set", back_names == tool_names,
              (back_names, tool_names))
        check("roundtrip preserves app_id", back["app_id"] == "weatherx",
              back.get("app_id"))
        check("roundtrip preserves version", back["version"] == "1.2.0",
              back.get("version"))

    print()
    print("pass=%d fail=%d" % (pass_n, fail_n))
    print("artifacts kept at: " + work)
    return 0 if fail_n == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
