#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Runner: 执行协议脚本、解析报错行、单行修补。

- run_one: 把带 `[NNN]` 前缀的协议文本解码成纯源码, 写入临时 .py 文件,
  用子进程 (即"Terminal") 执行, 捕获 stdout/stderr/返回码。
- 出错时, 用行号寻址协议把 traceback 里的物理行号映射回逻辑行号, 只抽取
  "出错行 + 前后上下文" 这几行作为回传给模型的 context, 极大节省 Token。
"""
import os
import subprocess
import tempfile
from dataclasses import dataclass, field

from . import lnum


@dataclass
class GeneratedScript:
    """一次生成/修复动作产出的协议脚本。"""
    protocol: str                       # 带 [NNN] 前缀的协议文本
    job_id: str = ""
    meta: dict = field(default_factory=dict)


@dataclass
class RunResult:
    job_id: str
    ok: bool
    returncode: int
    stdout: str
    stderr: str
    error_physical_line: "int | None" = None   # 纯源码里的物理行号
    error_logical_line: "int | None" = None     # 协议逻辑行号 (供模型重写)
    context: str = ""                           # 出错行 + 上下文 (协议文本)
    runtime_ms: float = 0.0


def _write_temp(protocol: str, job_id: str) -> str:
    decoded = lnum.decode(protocol)
    fd, path = tempfile.mkstemp(prefix="aio_%s_" % job_id, suffix=".py", text=True)
    with os.fdopen(fd, "w", encoding="utf-8") as fh:
        fh.write(decoded.source)
    return path


def run_one(job_id: str, protocol: str, timeout: float = 60.0,
            workdir: "str | None" = None) -> RunResult:
    """执行一个协议脚本, 返回 RunResult (含报错行定位与上下文)。

    workdir: 脚本的当前工作目录 (办公任务里通常是待处理文件所在目录)。
             为 None 时用脚本临时文件所在目录。
    """
    import time as _t
    path = None
    start = _t.time()
    try:
        path = _write_temp(protocol, job_id)
        cwd = workdir or os.path.dirname(path)
        # 必须显式指定 UTF-8: Windows 下 text=True 默认用系统 GBK, 脚本输出中文
        # 会在读取线程里抛 UnicodeDecodeError (导致 rc=1 且 stderr 为空)。
        proc = subprocess.run(
            [os.environ.get("AIO_PY", "python"), path],
            capture_output=True, text=True, timeout=timeout, cwd=cwd,
            encoding="utf-8", errors="replace",
            env={**os.environ, "PYTHONIOENCODING": "utf-8"},
        )
        rc = proc.returncode
        out, err = proc.stdout, proc.stderr
    except subprocess.TimeoutExpired as e:
        rc = -9
        out = (e.stdout or b"").decode("utf-8", "replace") if isinstance(e.stdout, (bytes,)) else (e.stdout or "")
        err = (e.stderr or b"").decode("utf-8", "replace") if isinstance(e.stderr, (bytes,)) else (e.stderr or "")
        out = out or ""
        err = err or "TimeoutExpired"
    finally:
        if path and os.path.exists(path):
            try:
                os.remove(path)
            except OSError:
                pass

    res = RunResult(job_id=job_id, ok=(rc == 0), returncode=rc,
                   stdout=out or "", stderr=err or "", runtime_ms=(_t.time() - start) * 1000)

    if not res.ok:
        phys = lnum.parse_traceback_error_line(res.stderr)
        res.error_physical_line = phys
        if phys is not None:
            decoded = lnum.decode(protocol)
            res.error_logical_line = lnum.map_physical_to_logical(decoded, phys)
            # 抽取上下文: 出错行 ±2 行 (仍是带行号的协议文本)
            res.context = _context_lines(protocol, res.error_logical_line, span=2)
    return res


def _context_lines(protocol: str, num: "int | None", span: int = 2) -> str:
    if num is None:
        return ""
    rows = protocol.split("\n")
    import re
    picked = []
    for raw in rows:
        m = re.match(r"^\s*\[(\d+)\]", raw)
        if not m:
            continue
        n = int(m.group(1))
        if num - span <= n <= num + span:
            picked.append(raw)
    return "\n".join(picked)


def apply_fix(protocol: str, num: int, new_line_protocol: str) -> str:
    """把模型返回的"单行修复"应用到协议文本, 返回新协议文本。

    new_line_protocol 形如 `[003] 修正后的完整行`。若模型返回里没有前缀,
    则按 num 直接替换该行正文。
    """
    import re
    m = re.match(r"^\s*\[(\d+)\]\s?(.*)$", new_line_protocol)
    if m:
        n = int(m.group(1))
        body = m.group(2)
        return lnum.patch(protocol, n, body)
    # 没有前缀: 当作正文直接替换
    return lnum.patch(protocol, num, new_line_protocol)
