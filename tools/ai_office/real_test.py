#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""real_test: 用真实 DeepSeek V4.1 Flash 验证 并发生成 + 行号寻址单行修复。

需要环境变量 AIO_API_KEY (或 --key)。会真的调用 DeepSeek API, 产生少量费用。

运行:
  set AIO_API_KEY=sk-xxxx
  python -m ai_office.real_test
"""
import json
import os
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))

from ai_office import models, runner, lnum  # noqa: E402
from ai_office.orchestrator import Orchestrator, JobSpec  # noqa: E402


TASKS = [
    ("t01", "读取 ./numbers.txt, 计算并打印总和、均值、最大值。"),
    ("t02", "读取 ./words.txt, 统计每个单词出现次数并按次数降序打印。"),
    ("t03", "给定一个整数列表, 打印其中所有素数。"),
    ("t04", "把字符串 'a,b;c d' 按 , ; 空格 三种分隔符切分并打印。"),
]


def _prepare_data():
    """准备真实测试用的输入文件目录 (模拟办公任务的数据目录)。"""
    d = os.path.join(tempfile.gettempdir(), "aio_real_data")
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, "numbers.txt"), "w", encoding="utf-8") as fh:
        fh.write("3 7 12 25 40\n")
    with open(os.path.join(d, "words.txt"), "w", encoding="utf-8") as fh:
        fh.write("apple banana apple cherry banana apple\n")
    return d


def main():
    if not os.environ.get("AIO_API_KEY"):
        print("缺少 AIO_API_KEY, 跳过真实测试")
        return 2
    backend = "openai"
    workdir = _prepare_data()
    print("== 真实 DeepSeek 测试 ==")
    print("后端: %s  模型: %s  数据目录: %s"
          % (backend, os.environ.get("AIO_MODEL", "deepseek-flash"), workdir))

    specs = [JobSpec(job_id=jid, prompt=p, max_fix_iters=2, workdir=workdir)
             for jid, p in TASKS]
    orch = Orchestrator(backend_name=backend, workers=4)

    t0 = time.time()
    outcomes = orch.run(specs)
    dt = time.time() - t0

    ok = sum(1 for o in outcomes if o.ok)
    fixes = sum(o.fix_iters for o in outcomes)
    print("\n并发执行: %d 个任务, %.1fs, 通过 %d/%d, 单行修复 %d 次"
          % (len(specs), dt, ok, len(specs), fixes))
    for o in outcomes:
        print("  [%s] %-4s runs=%d fixes=%d %s"
              % ("PASS" if o.ok else "FAIL", o.job_id, o.runs, o.fix_iters,
                 (o.stdout.strip().splitlines()[:2] if o.ok else [o.last_error[:100]])))

    # ---- 真实"行号寻址单行修复"演示 ----
    print("\n-- 行号寻址单行修复演示 --")
    be = models.get_backend(backend)
    # 让模型写一个会 NameError 的脚本 (故意用未定义变量), 再让它只修那一行
    proto = be.generate(
        "", "写一段 Python: 定义 x = 5, 然后 print(y * 2), 其中 y 故意未定义。"
            "只写 4 行以内。"
    )
    print("原始脚本:\n" + proto)
    res = runner.run_one("fixdemo", proto, timeout=30)
    print("\n首次执行 ok=%s, 报错逻辑行=%s" % (res.ok, res.error_logical_line))
    if not res.ok and res.error_logical_line:
        print("回传给模型的上下文 (仅 %d 字符, 整段为 %d 字符):\n%s"
              % (len(res.context), len(proto), res.context))
        fixed_line = be.fix_line(proto, res.error_logical_line, res.stderr[:600])
        print("\n模型只重写了这一行: %s" % fixed_line.strip())
        proto2 = runner.apply_fix(proto, res.error_logical_line, fixed_line)
        res2 = runner.run_one("fixdemo2", proto2, timeout=30)
        print("修复后 ok=%s stdout=%r" % (res2.ok, res2.stdout.strip()))
        if res2.ok:
            print("\n[OK] 真实模型的 行号寻址单行修复 成功")
            return 0
        print("\n[FAIL] 修复后仍失败")
        return 1
    print("\n[SKIP] 首次执行未报错 (模型可能写对了), 单行修复演示跳过")
    return 0 if ok == len(specs) else 1


if __name__ == "__main__":
    sys.exit(main())
