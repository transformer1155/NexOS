#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Orchestrator: 多进程并发执行 + 行号寻址调试闭环。

两个并发维度:
  1. 生成并发: 用 multiprocessing.Pool 让多个模型后端并行产出脚本
     (对应题目里"通过 MCP 让 DeepSeek V4.1 Flash 等模型并行生成多份脚本")。
  2. 执行并发: 生成后的脚本用独立子进程 (Terminal) 并发跑 —— run_one 本身
     每个脚本一个进程, 编排层不加全局串行锁, 自然并发。

调试闭环 (行号寻址协议):
  - 某脚本报错 -> 用 lnum 把物理行号映射回逻辑行号, 只抽取 ±2 行上下文。
  - 把"上下文 + 报错信息"回传给模型, 让它只重写那一逻辑行。
  - 用 lnum.patch / runner.apply_fix 就地替换那一行, 重跑。
  - 每脚本最多 max_fix_iters 次修复, 避免死循环。

典型用法见 cli.py 和 self_test.py。
"""
import multiprocessing as mp
from dataclasses import dataclass, field
from typing import Callable, Optional

from . import models, runner, lnum


@dataclass
class JobSpec:
    """一个办公任务 (例如"处理第 N 个 Excel")。"""
    job_id: str
    prompt: str                         # 给模型的任务描述
    system: str = ""                    # 可选额外系统提示
    timeout: float = 60.0
    max_fix_iters: int = 3
    workdir: "str | None" = None        # 脚本执行的工作目录 (待处理文件所在处)


@dataclass
class JobOutcome:
    job_id: str
    ok: bool
    protocol: str                      # 最终通过的协议脚本
    runs: int                          # 执行次数 (1 + 修复次数)
    fix_iters: int                     # 修复迭代次数
    debug_chars_sent: int = 0          # 调试阶段回传给模型的字符总量 (Token 近似)
    last_error: str = ""               # 最后一次报错 (若仍失败)
    stdout: str = ""


def _gen_one(args):
    """worker: 生成一份脚本 (在独立进程里调用模型后端)。"""
    spec_json, backend_name, backend_kw = args
    import json
    spec = JobSpec(**json.loads(spec_json))
    be = models.get_backend(backend_name, **backend_kw)
    protocol = be.generate(spec.system, spec.prompt)
    return spec.job_id, protocol


def _debug_and_run(spec: JobSpec, protocol: str, backend: models.ModelBackend,
                   progress: Optional[Callable] = None) -> JobOutcome:
    """执行 + 行号寻址修复闭环 (单脚本串行, 但多脚本之间并发)。"""
    runs = 0
    fix_iters = 0
    last_err = ""
    debug_chars = 0
    while True:
        res = runner.run_one(spec.job_id, protocol, timeout=spec.timeout,
                             workdir=spec.workdir)
        runs += 1
        if res.ok:
            return JobOutcome(job_id=spec.job_id, ok=True, protocol=protocol,
                              runs=runs, fix_iters=fix_iters,
                              debug_chars_sent=debug_chars, stdout=res.stdout)
        last_err = (res.stderr or "").strip()
        if fix_iters >= spec.max_fix_iters or res.error_logical_line is None:
            return JobOutcome(job_id=spec.job_id, ok=False, protocol=protocol,
                              runs=runs, fix_iters=fix_iters,
                              debug_chars_sent=debug_chars, last_error=last_err)
        # 行号寻址: 只重写出错的那一行
        fix_iters += 1
        # 协议的核心收益: 回传模型的只有"出错行 ±2 行"的上下文 (远小于整段重发)。
        # 报错文本与修复指令在两种方案里都要发, 不计入协议收益。
        chunk = len(res.context)
        debug_chars += chunk
        if progress:
            progress(spec.job_id, res.error_logical_line, res.context, chunk)
        new_line = backend.fix_line(protocol, res.error_logical_line,
                                    res.stderr[:800])
        protocol = runner.apply_fix(protocol, res.error_logical_line, new_line)


class Orchestrator:
    """并发编排器。"""

    def __init__(self, backend_name: str = "mock", backend_kw: Optional[dict] = None,
                 workers: int = 0):
        self.backend_name = backend_name
        self.backend_kw = backend_kw or {}
        self.workers = workers or min(mp.cpu_count(), 8)

    def run(self, specs: list[JobSpec],
            on_progress: Optional[Callable] = None) -> list[JobOutcome]:
        """并发生成 + 并发执行 + 行号寻址调试, 返回每个任务的结果。

        生成阶段用进程池并发; 执行/调试阶段在生成后并发启动 (每个脚本一个
        worker 进程负责"跑 + 修复"), 因此整体是多进程并发。
        """
        import json
        if not specs:
            return []

        # 1) 并发生成
        gen_args = [(json.dumps(s.__dict__, ensure_ascii=False), self.backend_name,
                     self.backend_kw) for s in specs]
        # 主进程里也持有一份后端 (用于调试阶段, 在 worker 内重建)
        if hasattr(mp, "get_context"):
            ctx = mp.get_context("spawn")
        else:
            ctx = mp
        with ctx.Pool(processes=min(self.workers, len(specs))) as pool:
            gen_results = pool.map(_gen_one, gen_args)

        protocols = {jid: proto for jid, proto in gen_results}
        spec_map = {s.job_id: s for s in specs}

        # 2) 并发执行 + 调试 (每个脚本一个进程, 内部串行修复)
        debug_args = [(json.dumps(spec_map[jid].__dict__, ensure_ascii=False),
                       protocols[jid], self.backend_name, self.backend_kw)
                      for jid in protocols]

        with ctx.Pool(processes=min(self.workers, len(specs))) as pool:
            outcomes = pool.map(_debug_one, debug_args)
        return list(outcomes)


def _debug_one(args):
    """worker: 重建后端并执行调试闭环。"""
    import json
    spec = JobSpec(**json.loads(args[0]))
    protocol = args[1]
    backend = models.get_backend(args[2], **(args[3] or {}))
    return _debug_and_run(spec, protocol, backend)
