#!/usr/bin/env python3
# =====================================================================
#  verify_all.py  -  NexOS 应用生态（.nexpack / 安装器 / MCP SDK）总验证
# ---------------------------------------------------------------------
#  一条命令跑完所有能在这台机器上自动验证的检查：
#
#    1. tools/nexpack.py          打包/校验/检视/解包          (e2e_nexpack.py)
#    2. tools/mcp_scan.py         构建期 [McpTool] 扫描         (e2e_nexpack.py)
#    3. PackageInstaller          安装/升级/拒绝/卸载/回滚      (installer_test)
#    4. MCP SDK 运行时行为        Invoke 派发 + JSON 处理       (mcp_sdk_test)
#    5. 内核 overlay 合并逻辑     apps.overlay.json + MERG      (verify_ai_overlay.py)
#
#  内核编译不在这里做（需要 g++/WSL），单独用：
#      wsl -d Ubuntu-24.04 -e bash /mnt/e/NexOS/NexOS/tools/check_kernel_ai.sh
#
#  用法: python tools/verify_all.py
# =====================================================================
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PY = sys.executable

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

results = []


def run_stage(name, args, cwd=None):
    print()
    print("=" * 70)
    print("  " + name)
    print("=" * 70)
    r = subprocess.run(args, cwd=cwd or ROOT, capture_output=True, text=True,
                       encoding="utf-8", errors="replace")
    out = (r.stdout or "") + (r.stderr or "")
    # 只回显尾部，避免刷屏
    tail = out.strip().splitlines()[-25:]
    for line in tail:
        print("  " + line)
    ok = (r.returncode == 0)
    results.append((name, ok))
    return ok


def main():
    print("NexOS .nexpack ecosystem verification")
    print("repo: " + ROOT)

    # 1 + 2: 包格式 + 构建期扫描（端到端）
    run_stage("1/5  .nexpack format + MCP build-time scan  (e2e)",
              [PY, os.path.join(ROOT, "tools", "e2e_nexpack.py")])

    # 3: 安装器行为
    run_stage("2/5  PackageInstaller behavior",
              ["dotnet", "run", "--project", "installer_test.csproj",
               "-c", "Release"],
              cwd=os.path.join(ROOT, "_nexpack_test", "installer_test"))

    # 4: MCP SDK 运行时行为
    run_stage("3/5  MCP SDK runtime behavior",
              ["dotnet", "run", "--project", "mcp_sdk_test.csproj",
               "-c", "Release"],
              cwd=os.path.join(ROOT, "_nexpack_test", "weatherbuild"))

    # 5: 内核 overlay 合并
    run_stage("4/5  kernel apps.overlay.json merge logic",
              [PY, os.path.join(ROOT, "tools", "verify_ai_overlay.py")])

    # 5: WinHost 整体编译
    run_stage("5/5  WinHost full build",
              ["dotnet", "build", "csharp/winhost/WinHost.csproj",
               "-c", "Release"])

    print()
    print("=" * 70)
    print("  SUMMARY")
    print("=" * 70)
    all_ok = True
    for name, ok in results:
        print("  %-52s %s" % (name, "PASS" if ok else "FAIL"))
        if not ok:
            all_ok = False
    print()
    if all_ok:
        print("ALL STAGES PASSED")
        print()
        print("Not covered here (needs a compiler / VM):")
        print("  * kernel TU compile:  wsl -d Ubuntu-24.04 -e bash \\")
        print("        /mnt/e/NexOS/NexOS/tools/check_kernel_ai.sh")
        print("  * full kernel image:  make BUILD=/tmp/nb")
        return 0
    print("SOME STAGES FAILED")
    return 1


if __name__ == "__main__":
    sys.exit(main())
