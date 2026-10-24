#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""CLI: ai_office 命令行入口。

子命令:
  self-test            离线自测 (并发 + 行号寻址单行修复 + Token 节省)
  run TASKS.json       读取任务清单 (JSON 数组), 多进程并发生成+执行+调试
  mcp                  启动 MCP server (stdio)

任务清单 JSON 格式 (run):
  [
    {"id": "excel-01", "prompt": "读取 ./data/01.xlsx 求每列均值",
     "timeout": 60, "max_fix_iters": 3},
    ...
  ]

环境变量: AIO_BACKEND / AIO_MODEL / AIO_BASE_URL / AIO_API_KEY / AIO_MCP_CMD
"""
import argparse
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))

from ai_office.orchestrator import Orchestrator, JobSpec  # noqa: E402


def cmd_self_test(_args):
    from ai_office import self_test
    return self_test.main()


def cmd_run(args):
    with open(args.tasks, "r", encoding="utf-8-sig") as fh:
        raw = json.load(fh)
    specs = [
        JobSpec(job_id=t["id"], prompt=t["prompt"],
                timeout=t.get("timeout", 60.0),
                max_fix_iters=t.get("max_fix_iters", 3),
                workdir=t.get("workdir") or args.workdir)
        for t in raw
    ]
    orch = Orchestrator(backend_name=os.environ.get("AIO_BACKEND", "mock"),
                        workers=args.workers)
    outcomes = orch.run(specs)
    ok = sum(1 for o in outcomes if o.ok)
    print("\n==== 结果 ====")
    for o in outcomes:
        status = "PASS" if o.ok else "FAIL"
        print("  [%s] %-12s runs=%d fix_iters=%d %s"
              % (status, o.job_id, o.runs, o.fix_iters,
                 ("out=%s" % o.stdout.strip()[:60]) if o.ok else o.last_error[:80]))
    print("通过 %d/%d" % (ok, len(outcomes)))
    return 0 if ok == len(outcomes) else 1


def cmd_mcp(_args):
    from ai_office import mcp_server
    mcp_server.main()
    return 0


def main():
    p = argparse.ArgumentParser(description="AI 办公并发执行 + 行号寻址协议工具箱")
    sub = p.add_subparsers(dest="cmd")
    sub.add_parser("self-test", help="离线自测")
    rp = sub.add_parser("run", help="并发执行任务清单 JSON")
    rp.add_argument("tasks", help="任务清单 JSON 文件")
    rp.add_argument("--workers", type=int, default=0, help="并发进程数 (0=自动)")
    rp.add_argument("--workdir", default=None,
                    help="脚本执行的工作目录 (待处理文件所在处)")
    sub.add_parser("mcp", help="启动 MCP server")
    args = p.parse_args()
    if args.cmd == "self-test" or args.cmd is None:
        return cmd_self_test(args)
    if args.cmd == "run":
        return cmd_run(args)
    if args.cmd == "mcp":
        return cmd_mcp(args)
    p.print_help()
    return 2


if __name__ == "__main__":
    sys.exit(main())
