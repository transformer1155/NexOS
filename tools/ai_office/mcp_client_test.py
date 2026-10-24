#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""mcp_client_test: 以 MCP 客户端身份 (stdio JSON-RPC) 驱动 ai_office MCP server。

验证"通过 MCP 让模型并行生成脚本 + 并发执行 + 行号寻址修复"这条链路。

运行:
  set AIO_API_KEY=sk-xxxx
  python -m ai_office.mcp_client_test
"""
import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(HERE)


def rpc(proc, obj, want_id):
    proc.stdin.write(json.dumps(obj) + "\n")
    proc.stdin.flush()
    while True:
        line = proc.stdout.readline()
        if not line:
            raise RuntimeError("MCP server 关闭了连接")
        line = line.strip()
        if not line:
            continue
        msg = json.loads(line)
        if msg.get("id") == want_id:
            return msg


def main():
    if not os.environ.get("AIO_API_KEY"):
        print("缺少 AIO_API_KEY, 跳过")
        return 2

    data = os.path.join(tempfile.gettempdir(), "aio_real_data")
    os.makedirs(data, exist_ok=True)
    open(os.path.join(data, "numbers.txt"), "w", encoding="utf-8").write("3 7 12 25 40\n")
    open(os.path.join(data, "words.txt"), "w", encoding="utf-8").write("apple banana apple\n")

    env = dict(os.environ)
    env["PYTHONIOENCODING"] = "utf-8"
    proc = subprocess.Popen(
        [sys.executable, "-m", "ai_office.mcp_server"],
        cwd=TOOLS, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL, text=True, encoding="utf-8", env=env,
    )
    try:
        init = rpc(proc, {"jsonrpc": "2.0", "id": 1, "method": "initialize",
                          "params": {"protocolVersion": "2024-11-05",
                                     "capabilities": {},
                                     "clientInfo": {"name": "test", "version": "1"}}}, 1)
        print("initialize ok:", init["result"]["serverInfo"])

        tools = rpc(proc, {"jsonrpc": "2.0", "id": 2, "method": "tools/list",
                           "params": {}}, 2)
        names = [t["name"] for t in tools["result"]["tools"]]
        print("tools:", names)

        # 1) 单个生成
        gen = rpc(proc, {"jsonrpc": "2.0", "id": 3, "method": "tools/call",
                         "params": {"name": "ai_generate",
                                    "arguments": {"user": "写 3 行 Python: 打印 1 到 3 的和"}}}, 3)
        text = gen["result"]["content"][0]["text"]
        print("\nai_generate 返回 %d 字符, 前两行:" % len(text))
        print("\n".join(text.splitlines()[:2]))

        # 2) 并发执行 (ai_run)
        tasks = [
            {"id": "m01", "prompt": "读取 ./numbers.txt, 打印总和", "workdir": data},
            {"id": "m02", "prompt": "读取 ./words.txt, 打印单词总数", "workdir": data},
        ]
        run = rpc(proc, {"jsonrpc": "2.0", "id": 4, "method": "tools/call",
                         "params": {"name": "ai_run",
                                    "arguments": {"tasks": tasks, "workers": 2}}}, 4)
        outcomes = json.loads(run["result"]["content"][0]["text"])
        print("\nai_run 并发结果:")
        all_ok = True
        for o in outcomes:
            all_ok = all_ok and o["ok"]
            print("  [%s] %s runs=%d fixes=%d out=%r"
                  % ("PASS" if o["ok"] else "FAIL", o["job_id"], o["runs"],
                     o["fix_iters"], o["stdout"].strip()[:60]))
        print("\n[%s] MCP 链路 (生成/并发执行/行号修复) 验证完成"
              % ("OK" if all_ok else "PARTIAL"))
        return 0 if all_ok else 1
    finally:
        try:
            proc.stdin.close()
        except Exception:
            pass
        proc.kill()


if __name__ == "__main__":
    sys.exit(main())
