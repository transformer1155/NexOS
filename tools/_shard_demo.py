#!/usr/bin/env python3
# _shard_demo.py —— 分布式推理编排的端到端验证（含掉线容错）
#
# 起 3 个分片节点(算力权重不同) + 1 个编排器，跑一次分片推理；
# 中途 kill 掉一个节点，验证自动重新分片 + 从检查点续跑。
import subprocess, sys, time, os, signal, socket

HERE = os.path.dirname(os.path.abspath(__file__))
PY = sys.executable
PORTS = [5501, 5502, 5503]
WEIGHTS = {5501: 1, 5502: 2, 5503: 4}      # 算力不同 -> 动态分片应按 1:2:4 分
LAYERS, DIM = 12, 16

procs = []
try:
    print("=== 1) 启动 3 个分片节点（算力权重 1 / 2 / 4）===")
    for p in PORTS:
        f = open(os.path.join(HERE, f"_shard_{p}.log"), "w")
        procs.append(subprocess.Popen(
            [PY, os.path.join(HERE, "distnet_shard_node.py"),
             "--port", str(p), "--weight", str(WEIGHTS[p]),
             "--dim", str(DIM), "--orch-port", "5500", "--delay", "0.5"],
            stdout=f, stderr=subprocess.STDOUT))
        time.sleep(0.3)

    print("=== 2) 启动编排器并跑一次分片推理（12 层 / 3 节点）===")
    orch = subprocess.Popen(
        [PY, os.path.join(HERE, "distnet_shard_orchestrator.py"),
         "--port", "5500", "--layers", str(LAYERS), "--dim", str(DIM),
         "--nodes", ",".join(f"127.0.0.1:{p}" for p in PORTS)],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    time.sleep(3.0)

    # 时序（每层 0.5s）：5501 L0-0 约 0.5s 完成 → 5502 L1-3 约 2.0s 完成
    # → 5503 L4-11 从 2.0s 算到 ~6.0s。所以在 3.0s 时 5503 正在算，kill 它才能
    # 真正触发“掉线容错”（kill 已交棒的节点不会触发，因为那不叫掉线）。
    print("=== 3) 模拟节点掉线：kill 5503（持有 L4-11，此时正在计算中）===")
    for i, p in enumerate(PORTS):
        if p == 5503:
            try:
                procs[i].kill()
            except Exception:
                pass
    # 留出足够时间：检测掉线(~2s) + 重新分片 + 存活节点重算剩余层
    time.sleep(14.0)

    try:
        rc = orch.wait(timeout=20)
    except subprocess.TimeoutExpired:
        orch.kill(); rc = "killed(timeout)"
    out = orch.stdout.read() if orch.stdout else ""
    print("----- orchestrator output (rc=%s) -----" % rc)
    print(out)
    print("----- node logs -----")
    for p in PORTS:
        path = os.path.join(HERE, f"_shard_{p}.log")
        if os.path.exists(path):
            with open(path) as f:
                print(f"--- node {p} ---")
                print(f.read())
finally:
    for p in procs:
        try:
            p.kill()
        except Exception:
            pass
    try:
        orch.kill()
    except Exception:
        pass
