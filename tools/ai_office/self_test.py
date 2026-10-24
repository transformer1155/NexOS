#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""self_test: 离线验证 行号寻址协议 + 并发执行 + Token 节省的"单行修复"。

不依赖任何网络/API。用 MockBackend 故意留一个 bug, 演示:
  1) 多进程并发生成 + 并发执行 (10 个任务并行, 验证有真实并发);
  2) 报错时只回传"出错行 + 上下文"给模型, 模型只重写那一行;
  3) 修复后脚本通过, 且全程回传给模型的 token 远小于"整段重发"。

运行: python -m ai_office.self_test
"""
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))

from ai_office import models, runner, lnum, orchestrator  # noqa: E402
from ai_office.orchestrator import JobSpec, Orchestrator  # noqa: E402


def test_lnum_roundtrip():
    src = "import sys\ndef f():\n    return 1"
    proto = lnum.encode(src)
    dec = lnum.decode(proto)
    assert dec.source == src, "encode/decode 往返不一致"
    assert dec.lines[0].num == 1 and dec.lines[-1].num == 3
    print("  [OK] lnum encode/decode 往返")


def test_lnum_patch():
    proto = lnum.encode("a = 1\nb = 2\nc = a - b\n")
    new = lnum.patch(proto, 3, "c = a + b")
    dec = lnum.decode(new)
    assert "c = a + b" in dec.source
    assert "c = a - b" not in dec.source
    print("  [OK] lnum 单行 patch 仅替换目标行")


def test_concurrency_and_single_line_debug():
    N = 10
    specs = [
        JobSpec(job_id="task-%02d" % i,
                prompt="__BUG_AT_3__ process excel #%d" % i,
                max_fix_iters=3)
        for i in range(N)
    ]
    orch = Orchestrator(backend_name="mock", workers=4)

    t0 = time.time()
    outcomes = orch.run(specs)   # progress 回调在子进程内, 不跨进程聚合; 用 outcome 自带的统计
    dt = time.time() - t0

    passed = sum(1 for o in outcomes if o.ok)
    total_fix_iters = sum(o.fix_iters for o in outcomes)
    # 行号寻址下, 调试阶段回传给模型的真实字符总量 (Token 近似)
    ctx_chars = sum(o.debug_chars_sent for o in outcomes)
    # 若"整段重发": 每次修复都把整个脚本回传给模型
    avg_script_len = sum(len(o.protocol) for o in outcomes) / max(1, len(outcomes))
    naive_chars = int(total_fix_iters * avg_script_len)
    saved = max(0, naive_chars - ctx_chars)

    print("  [并发] %d 个任务, %.2fs 完成, workers=4" % (N, dt))
    print("  [通过] %d/%d  总修复迭代 %d 次" % (passed, N, total_fix_iters))
    print("  [Token] 单行修复回传约 %d 字符; 若整段重发约 %d 字符 -> 节省约 %d (%.0f%%)"
          % (ctx_chars, naive_chars, saved,
             100.0 * saved / naive_chars if naive_chars else 0))

    assert passed == N, "并非所有任务都通过"
    assert total_fix_iters > 0, "未触发单行修复"
    assert ctx_chars < naive_chars, "单行修复未节省 Token"
    print("  [OK] 并发执行 + 行号寻址单行修复 + Token 节省 全部验证通过")


def main():
    print("== ai_office self_test ==")
    test_lnum_roundtrip()
    test_lnum_patch()
    test_concurrency_and_single_line_debug()
    print("== ALL PASS ==")
    return 0


if __name__ == "__main__":
    sys.exit(main())
