#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""ai_office: 面向 AI 办公任务的并发执行 + 行号寻址协议工具箱。

本包实现两件事:
  1. 多进程并发执行 —— 通过 MCP 让多个模型并行生成多份脚本, 丢入各子进程
     (Terminal) 并发测试, 失败的只走"行号寻址协议"做最小化重写, 极大节省 Token。
  2. 行号寻址协议 —— AI 生成代码时给每一行加 `[001]` 前缀编号; Debug 时系统
     精准定位报错行号, 只把那一行(及其上下文)回传给模型重写, 而不是整段重来。

全部使用 Python 标准库, 无第三方依赖, 可在 Windows / Linux 直接运行。
"""
from . import lnum
from . import models
from . import runner
from . import orchestrator
from .runner import GeneratedScript, RunResult, run_one
from .orchestrator import Orchestrator, JobSpec

__all__ = [
    "lnum", "models", "runner", "orchestrator",
    "GeneratedScript", "RunResult", "run_one", "Orchestrator", "JobSpec",
]

__version__ = "1.0.0"
