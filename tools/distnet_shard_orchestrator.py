#!/usr/bin/env python3
# distnet_shard_orchestrator.py —— 分布式推理编排器（分配端）
#
# 对应 5 项目标：
#   1. 模型分片        —— 把总层数按权重切成连续区间，分给不同节点
#   2. 分布式推理      —— 激活值沿节点链逐段前传，最后一个节点产出结果
#   3. 动态负载均衡    —— 分片边界按各节点 weight(算力) 比例划分，算力大的多分
#   4. 中间结果传输    —— AACT 消息在节点间传激活值；CKPT 回传检查点
#   5. 推理容错        —— 心跳探测；节点掉线则重新分片并从最近检查点续跑
#
# 线协议(文本 UDP，与 distnet 风格一致):
#   QUERY                                  -> BEACON <role> <weight>
#   SHARD <job> <s> <e> <nh> <np> <last>   -> SHARDOK <job> <port>
#   AACT  <job> <seq> <floats...>          节点间激活值传递
#   CKPT  <job> <seq> <end_layer> <vec>    节点 -> 调度器，检查点
#   OUT   <job> <seq> <vec>                末节点 -> 调度器，最终结果
#   PING  <job>                            -> PONG <job> <end_layer>
#
# 注意：socket 只能有一个读取者。所有回包统一由 serve() 线程收取并分发，
#       发现/心跳通过事件等待，避免多线程 recvfrom 抢包。
#
# 用法: python distnet_shard_orchestrator.py --nodes 127.0.0.1:5501,127.0.0.1:5502 \
#            --layers 12 --dim 16
import argparse, os, socket, threading, time

FAIL_TIMEOUT = 1.0      # 心跳超时(秒)
MAX_MISS     = 2        # 连续多少次无响应判定掉线
DISC_TIMEOUT = 1.5      # 发现等待(秒)


class Orchestrator:
    def __init__(self, host, port, nodes, total_layers, dim, seed):
        self.host, self.port = host, port
        self.nodes = list(nodes)
        self.total_layers = total_layers
        self.dim, self.seed = dim, seed
        self.weights = {}                     # addr -> weight
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind((host, port))
        # 事件与状态
        self.jobs = {}                        # job -> state
        self._pong = {}                       # addr -> threading.Event
        self._disc_ev = threading.Event()
        self.lock = threading.Lock()
        self.log(f"orchestrator up @{host}:{port} layers={total_layers} dim={dim}")

    def log(self, *a):
        print("[orch]", *a, flush=True)

    # ---------------- 唯一的收包线程 ----------------
    def serve(self):
        while True:
            try:
                data, addr = self.sock.recvfrom(65535)
            except (ConnectionResetError, OSError):
                # Windows 陷阱：给“已挂掉的节点”发过 UDP 后对端回 ICMP 端口不可达，
                # 下一次 recvfrom 会抛 WSAECONNRESET。若在此 break，收包线程会退出，
                # 编排器从此“失聪”——刚好在最需要容错的时刻收不到恢复结果。必须继续。
                continue
            try:
                self._dispatch(data.decode(errors="replace").split(), addr)
            except Exception as e:              # 单个坏包不能打死收包线程
                self.log("packet error:", e)

    def _dispatch(self, t, addr):
            if not t:
                return
            cmd = t[0]
            if os.environ.get("SHARD_DEBUG"):
                self.log(f"<- {cmd} from {addr[1]} ({len(t)} tok)")
            if cmd == "BEACON":                       # 发现回包
                w = int(t[2]) if len(t) > 2 and t[2].isdigit() else 1
                with self.lock:
                    self.weights[addr] = w
                self._disc_ev.set()
            elif cmd == "PONG":                       # 心跳回包
                ev = self._pong.get(addr)
                if ev:
                    ev.set()
            elif cmd == "CKPT" and len(t) >= 4:       # CKPT job seq end_layer vec...
                st = self.jobs.get(t[1])
                if st is not None:
                    st["ckpt"][int(t[3])] = [float(x) for x in t[4:4 + self.dim]]
                    st.setdefault("done_nodes", set()).add(addr)   # 已交棒，免探测
            elif cmd == "OUT" and len(t) >= 3:        # OUT job seq vec...
                st = self.jobs.get(t[1])
                if st is not None and st["out"] is None:
                    st["out"] = [float(x) for x in t[3:3 + self.dim]]
                    self.log(f"job {t[1]} done ({len(st['plan'])} shards)")

    # ---------------- 1) 发现 ----------------
    def discover(self):
        with self.lock:
            self.weights = {}
        self._disc_ev.clear()
        for a in self.nodes:
            try:
                self.sock.sendto(b"QUERY", a)
            except OSError:
                pass
        end = time.time() + DISC_TIMEOUT
        while time.time() < end:
            with self.lock:
                got = len(self.weights)
            if got >= len(self.nodes):
                break
            self._disc_ev.wait(0.1)
            self._disc_ev.clear()
        with self.lock:
            found = list(self.weights.keys())
        if not found:
            self.log("no shard nodes discovered")
        else:
            self.log("discovered: " + ", ".join(f"{a[1]}(w={self.weights[a]})" for a in found))
        return found

    # ---------------- 3) 动态负载均衡：按算力权重切层 ----------------
    def _plan(self, addrs, start_layer=0):
        ws = [max(1, self.weights.get(a, 1)) for a in addrs]
        tot = sum(ws)
        total = self.total_layers - start_layer
        plan, cur = [], start_layer
        for i, a in enumerate(addrs):
            n = total * ws[i] // tot
            if i == len(addrs) - 1:                   # 末段吃下余数，保证不漏层
                n = self.total_layers - cur
            if n <= 0:
                n = 1
            plan.append((a, cur, cur + n - 1, i == len(addrs) - 1))
            cur += n
        return plan

    def assign(self, job, plan):
        orch = (self.host, self.port)
        for i, (addr, s, e, last) in enumerate(plan):
            nxt = orch if last else plan[i + 1][0]
            self.sock.sendto(
                f"SHARD {job} {s} {e} {nxt[0]} {nxt[1]} {1 if last else 0}".encode(), addr)
        self.log(f"job {job} shards: " + " | ".join(f"{a[1]}:L{s}-{e}" for a, s, e, _ in plan))

    # ---------------- 2) 分布式推理 ----------------
    def infer(self, job, vec, timeout=30.0):
        addrs = self.discover()
        if not addrs:
            return None, "no nodes"
        plan = self._plan(addrs)
        st = {"plan": plan, "ckpt": {}, "out": None, "vec": list(vec), "seq": 0, "stop": False}
        self.jobs[job] = st
        self.assign(job, plan)
        self.sock.sendto(self._aact(job, 0, vec).encode(), plan[0][0])
        # 边跑边做心跳容错
        t = threading.Thread(target=self._monitor, args=(job, st), daemon=True)
        t.start()
        end = time.time() + timeout
        while time.time() < end:
            if st["out"] is not None:
                st["stop"] = True
                return st["out"], "ok"
            time.sleep(0.05)
        st["stop"] = True
        return None, "timeout"

    def _aact(self, job, seq, vec):
        return f"AACT {job} {seq} " + " ".join(f"{v:.6f}" for v in vec)

    # ---------------- 5) 推理容错 ----------------
    def _monitor(self, job, st):
        miss = {}
        while not st["stop"] and st["out"] is None:
            time.sleep(0.4)
            # 每轮重新读 plan：_recover 会替换它，用旧快照会反复探测已死节点
            for (addr, s, e, last) in list(st["plan"]):
                if st["out"] is not None or st["stop"]:
                    return
                if st.get("done_nodes") and addr in st["done_nodes"]:
                    continue                    # 该分片已交棒，不再探测
                ok = self._ping(addr, job)
                miss[addr] = 0 if ok else miss.get(addr, 0) + 1
                if miss[addr] >= MAX_MISS:
                    self.log(f"!! node {addr[1]} (L{s}-{e}) dead -> reshard")
                    self._recover(job, st, addr)
                    miss[addr] = 0
                    break

    def _ping(self, addr, job):
        ev = self._pong.setdefault(addr, threading.Event())
        ev.clear()
        try:
            self.sock.sendto(f"PING {job}".encode(), addr)
        except OSError:
            return False
        return ev.wait(FAIL_TIMEOUT)

    def _recover(self, job, st, dead):
        """剔除死节点 → 存活节点重新分片 → 从死节点之前最近的检查点续跑。"""
        survivors = [a for (a, s, e, l) in st["plan"] if a != dead]
        if not survivors:
            self.log("all shard nodes lost; job failed")
            st["out"] = []
            return
        dead_start = next((s for (a, s, e, l) in st["plan"] if a == dead), 0)
        cands = [L for L in st["ckpt"] if L < dead_start]
        if cands:
            resume_layer = max(cands)
            vec = st["ckpt"][resume_layer]
            self.log(f"resume from checkpoint layer {resume_layer}")
        else:
            resume_layer = -1
            vec = st["vec"]
            self.log("no checkpoint before failure; restart from input")
        st["plan"] = self._plan(survivors, start_layer=resume_layer + 1)
        st["seq"] += 1
        self.assign(job, st["plan"])
        self.sock.sendto(self._aact(job, st["seq"], vec).encode(), st["plan"][0][0])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=5500)
    ap.add_argument("--nodes", required=True, help="逗号分隔 ip:port")
    ap.add_argument("--layers", type=int, default=12)
    ap.add_argument("--dim", type=int, default=16)
    ap.add_argument("--seed", type=int, default=1)
    a = ap.parse_args()
    nodes = [(s.split(":")[0], int(s.split(":")[1])) for s in a.nodes.split(",")]
    o = Orchestrator(a.host, a.port, nodes, a.layers, a.dim, a.seed)
    threading.Thread(target=o.serve, daemon=True).start()
    time.sleep(0.3)
    vec = [0.1 * (i + 1) for i in range(a.dim)]
    out, why = o.infer("job1", vec)
    print("RESULT:", why, "->", None if out is None else [round(v, 4) for v in out], flush=True)


if __name__ == "__main__":
    main()
