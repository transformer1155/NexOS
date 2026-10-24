#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""标准库实现的 MCP server (stdio JSON-RPC), 暴露 AI 办公并发执行工具。

工具 (tools):
  - ai_generate  通过模型后端生成【带行号前缀】的脚本协议文本。
  - ai_fix_line  只重写脚本里出错的那一行 (行号寻址协议)。
  - ai_run       解码并并发执行脚本, 自动走"行号寻址调试闭环"。

这就是题目里"通过 MCP 让模型并行生成多份脚本, 丢入 Terminal 并发测试"的入口:
编排器 (orchestrator) 每个任务一次 tools/call -> 模型生成 -> 子进程执行 ->
报错回传 -> 单行重写。

启动: python -m ai_office.mcp_server
依赖: 仅标准库。
"""
import json
import sys
import threading

from . import models, runner, lnum
from .orchestrator import Orchestrator, JobSpec


TOOLS = [
    {
        "name": "ai_generate",
        "description": "Generate a line-numbered (protocol) Python script via a model backend. "
                       "Returns text with [NNN] line prefixes.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "user": {"type": "string", "description": "task prompt for the model"},
                "system": {"type": "string", "description": "optional extra system prompt"},
                "backend": {"type": "string", "description": "mock|openai|mcp (default env)"},
            },
            "required": ["user"],
        },
    },
    {
        "name": "ai_fix_line",
        "description": "Rewrite ONLY the numbered line that errored (line-number addressing). "
                       "Returns a single [NNN] line.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "protocol": {"type": "string", "description": "current script with [NNN] prefixes"},
                "num": {"type": "integer", "description": "logical line number to rewrite"},
                "error_context": {"type": "string", "description": "error/traceback text"},
                "backend": {"type": "string"},
            },
            "required": ["protocol", "num", "error_context"],
        },
    },
    {
        "name": "ai_run",
        "description": "Run multiple generated scripts concurrently (subprocess per script) and "
                       "auto-fix errors using the line-number addressing protocol. "
                       "tasks: list of {id, prompt, timeout?, max_fix_iters?}.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "tasks": {
                    "type": "array",
                    "items": {
                        "type": "object",
                        "properties": {
                            "id": {"type": "string"},
                            "prompt": {"type": "string"},
                            "timeout": {"type": "number"},
                            "max_fix_iters": {"type": "integer"},
                            "workdir": {"type": "string",
                                        "description": "script working directory"},
                        },
                        "required": ["id", "prompt"],
                    },
                },
                "backend": {"type": "string"},
                "workers": {"type": "integer"},
            },
            "required": ["tasks"],
        },
    },
]


class MCPServer:
    def __init__(self, inp, outp):
        self.inp = inp
        self.outp = outp
        self.id_lock = threading.Lock()
        self._shutdown = threading.Event()

    def _send(self, obj):
        self.outp.write(json.dumps(obj, ensure_ascii=False) + "\n")
        self.outp.flush()

    def _reply(self, req_id, result):
        self._send({"jsonrpc": "2.0", "id": req_id, "result": result})

    def _error(self, req_id, code, message):
        self._send({"jsonrpc": "2.0", "id": req_id,
                    "error": {"code": code, "message": message}})

    # -- tool dispatch -------------------------------------------------------
    def call_tool(self, name, args):
        if name == "ai_generate":
            be = models.get_backend(args.get("backend"))
            proto = be.generate(args.get("system", ""), args["user"])
            return {"content": [{"type": "text", "text": proto}]}
        if name == "ai_fix_line":
            be = models.get_backend(args.get("backend"))
            line = be.fix_line(args["protocol"], int(args["num"]),
                               args.get("error_context", ""))
            return {"content": [{"type": "text", "text": line}]}
        if name == "ai_run":
            specs = [
                JobSpec(
                    job_id=t["id"],
                    prompt=t["prompt"],
                    timeout=t.get("timeout", 60.0),
                    max_fix_iters=t.get("max_fix_iters", 3),
                    workdir=t.get("workdir"),
                )
                for t in args["tasks"]
            ]
            orch = Orchestrator(backend_name=args.get("backend"),
                                workers=args.get("workers", 0))
            outcomes = orch.run(specs)
            payload = [
                {
                    "job_id": o.job_id,
                    "ok": o.ok,
                    "runs": o.runs,
                    "fix_iters": o.fix_iters,
                    "debug_chars_sent": o.debug_chars_sent,
                    "protocol": o.protocol,
                    "stdout": o.stdout,
                    "last_error": o.last_error,
                }
                for o in outcomes
            ]
            return {"content": [{"type": "text",
                                 "text": json.dumps(payload, ensure_ascii=False)}]}
        raise ValueError("unknown tool: %s" % name)

    # -- main loop -----------------------------------------------------------
    def serve(self):
        for line in self.inp:
            line = line.strip()
            if not line:
                continue
            try:
                req = json.loads(line)
            except json.JSONDecodeError:
                continue
            method = req.get("method")
            req_id = req.get("id")
            if method == "initialize":
                self._reply(req_id, {
                    "protocolVersion": "2024-11-05",
                    "capabilities": {"tools": {}},
                    "serverInfo": {"name": "ai_office", "version": "1.0.0"},
                })
            elif method == "notifications/initialized":
                continue
            elif method == "tools/list":
                self._reply(req_id, {"tools": TOOLS})
            elif method == "tools/call":
                params = req.get("params", {})
                try:
                    result = self.call_tool(params.get("name"), params.get("arguments", {}))
                    self._reply(req_id, result)
                except Exception as e:  # noqa
                    self._error(req_id, -32000, str(e))
            elif method == "shutdown":
                self._reply(req_id, None)
            elif method == "exit":
                break
            else:
                if req_id is not None:
                    self._error(req_id, -32601, "method not found: %s" % method)


def main():
    # MCP 通过 stdio 交换 JSON, 必须保证 UTF-8, 否则 Windows 默认 codepage 会把
    # 含中文的回复 JSON 写坏。优先用 TextIOWrapper 重绑 stdout/stderr。
    try:
        if hasattr(sys.stdout, "reconfigure"):
            sys.stdout.reconfigure(encoding="utf-8")
            sys.stderr.reconfigure(encoding="utf-8")
    except Exception:  # noqa
        pass
    import io
    out = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8", write_through=True)
    srv = MCPServer(sys.stdin, out)
    srv.serve()


if __name__ == "__main__":
    main()
