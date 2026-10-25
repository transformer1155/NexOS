#!/usr/bin/env python3
# distnet_shard_node.py —— 分布式推理的“分片节点”
#
# 每个节点持有模型的一个**连续层区间**（Model Sharding）。推理时：
#   上一跳的激活值(AACT)进来 → 本节点算完自己那几层 → 发给下一跳
#   最后一个节点把结果(OUT)回给调度器；每个节点同时把“出口激活值”(CKPT)
#   回传调度器做检查点，用于掉线容错时从中断处续跑。
#
# 层计算目前是**确定性合成层**：act = tanh(W_i @ act + b_i)，权重由 (seed, layer)
# 生成，多层串联即一个小的前馈栈。真实 GGUF 权重嵌入后，只需把 _apply_layers()
# 换成真实层算子，分片/激活路由/容错这套机制完全不用改。
#
# 用法: python distnet_shard_node.py --port 5501 --weight 4 [--dim 16] [--seed 1]
import argparse, math, socket, sys, threading, time

# ---- 合成层：由 seed 决定的确定性权重，保证多节点/多次运行结果一致 ----
def _layer_params(dim, layer, seed):
    w, b = [], []
    for i in range(dim):
        row = []
        for j in range(dim):
            # 轻量确定性伪随机（避免依赖 numpy）
            h = (seed * 2654435761 + layer * 40503 + i * 97 + j * 31) & 0xFFFFFFFF
            h ^= h >> 15; h = (h * 2246822519) & 0xFFFFFFFF; h ^= h >> 13
            row.append(((h % 2000) / 1000.0) - 1.0)
        w.append(row)
        hb = (seed * 97 + layer * 131 + i * 17) & 0xFFFFFFFF
        b.append(((hb % 400) / 1000.0) - 0.2)
    return w, b

_WCACHE = {}
def apply_layers(vec, start, end, dim, seed, delay=0.0):
    """对 vec 依次施加第 start..end 层（含），返回新激活值。"""
    cur = list(vec)
    for L in range(start, end + 1):
        if delay:
            time.sleep(delay)
        key = (L, dim, seed)
        w, b = _WCACHE.get(key) or (lambda p: (_WCACHE.setdefault(key, p), p)[1])(_layer_params(dim, L, seed))
        out = []
        for i in range(dim):
            s = b[i]
            row = w[i]
            for j in range(dim):
                s += row[j] * cur[j]
            out.append(math.tanh(s))
        cur = out
    return cur


class ShardNode:
    def __init__(self, host, port, weight, dim, seed, orch, delay=0.0):
        self.host, self.port, self.weight, self.dim, self.seed = host, port, weight, dim, seed
        self.delay = delay          # 每层人为延时(秒)，便于演示/测试中途掉线
        self.orch = tuple(orch)                 # (ip, port) 调度器地址
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind((host, port))
        # 当前被分配的层区间与下一跳
        self.job = None; self.start = 0; self.end = -1
        self.next_hop = None; self.is_last = False
        self.busy = False
        self.log(f"shard node up @{host}:{port} weight={weight} dim={dim}")

    def log(self, *a):
        print(f"[node:{self.port}]", *a, flush=True)

    def send(self, addr, msg):
        self.sock.sendto(msg.encode(), addr)

    def serve(self):
        while True:
            try:
                data, addr = self.sock.recvfrom(65535)
            except (ConnectionResetError, OSError):
                # 与编排器同样的 Windows 陷阱:给已挂掉的下一跳发过 UDP 后,对端回
                # ICMP 端口不可达,下一次 recvfrom 会抛 WSAECONNRESET。若在此退出,
                # 一个节点掉线会“传染”导致整条链上的节点接连退出。必须继续。
                continue
            try:
                self.handle(data.decode(errors="replace").strip(), addr)
            except Exception as e:
                self.log("error:", e)       # 单包异常不能打死节点

    def handle(self, msg, addr):
        t = msg.split()
        if not t:
            return
        cmd = t[0]

        if cmd == "QUERY":                      # 发现（与内核 distnet 同样的语义）
            self.send(addr, f"BEACON compute {self.weight}")
            return

        if cmd == "PING":
            # 心跳必须**立即**回：计算在工作线程里跑，忙碌也要应答，
            # 否则调度器会把“正在算重活”的节点误判为掉线。
            job = t[1] if len(t) > 1 else "-"
            self.send(addr, f"PONG {job} {self.end} {'busy' if self.busy else 'idle'}")
            return

        if cmd == "SHARD":                      # SHARD <job> <start> <end> <next_host> <next_port> <is_last>
            self.job = t[1]; self.start = int(t[2]); self.end = int(t[3])
            nh, np_, last = t[4], int(t[5]), t[6] == "1"
            self.next_hop = (nh, np_)
            self.is_last = last
            self.log(f"assigned layers {self.start}..{self.end} next={nh}:{np_} last={last}")
            self.send(self.orch, f"SHARDOK {self.job} {self.port}")
            return

        if cmd == "AACT":                       # AACT <job> <seq> <floats...>
            job, seq = t[1], int(t[2])
            vec = [float(x) for x in t[3:3 + self.dim]]
            if len(vec) != self.dim:
                self.log("bad activation dim"); return
            # 计算放到工作线程：主线程继续收 UDP（心跳/查询不会被重活阻塞）
            s, e, nxt, last = self.start, self.end, self.next_hop, self.is_last
            self.busy = True
            def work():
                try:
                    out = apply_layers(vec, s, e, self.dim, self.seed, self.delay)
                    payload = " ".join(f"{v:.6f}" for v in out)
                    self.send(self.orch, f"CKPT {job} {seq} {e} {payload}")   # 检查点
                    if last:
                        self.send(self.orch, f"OUT {job} {seq} {payload}")
                        self.log(f"job {job} seq {seq}: produced final output")
                    else:
                        self.send(nxt, f"AACT {job} {seq} {payload}")
                        self.log(f"job {job} seq {seq}: layers {s}..{e} -> next {nxt[1]}")
                finally:
                    self.busy = False
            threading.Thread(target=work, daemon=True).start()
            return

        self.log("unknown:", cmd)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--weight", type=int, default=4)      # 算力权重（核数）
    ap.add_argument("--dim", type=int, default=16)        # 激活值维度
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--orch-host", default="127.0.0.1")
    ap.add_argument("--orch-port", type=int, default=5500)
    ap.add_argument("--delay", type=float, default=0.0, help="每层延时(秒),便于测试容错")
    a = ap.parse_args()
    ShardNode(a.host, a.port, a.weight, a.dim, a.seed,
              (a.orch_host, a.orch_port), a.delay).serve()


if __name__ == "__main__":
    main()
