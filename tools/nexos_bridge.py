#!/usr/bin/env python3
"""NexOS ops-panel host bridge (pure standard library, no third-party deps)."""
import base64, hashlib, os, socket, struct, subprocess, sys, threading, time

SERIAL_HOST = "127.0.0.1"
SERIAL_PORT = int(os.environ.get("SERIAL_PORT", "4321"))
WS_HOST = "0.0.0.0"
WS_PORT = int(os.environ.get("WS_PORT", "8765"))
GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

HERE = os.path.dirname(os.path.abspath(__file__))

# ---- Forge 本地服务路由 ----------------------------------------------------
# Agent Forge 的“分布式推理”节点需要宿主侧的分片推理能力(内核区已无空间放分片
# 引擎)。桥在这里充当协议翻译/路由层:以 `forge ` 开头的指令不下发内核,而是交给
# 本地的分片编排器执行,再把结果按内核输出的格式回传给浏览器。
SHARD_PORTS   = [5501, 5502, 5503]
SHARD_WEIGHTS = {5501: 1, 5502: 2, 5503: 4}   # 算力权重 -> 动态分片比例
SHARD_ORCH_PORT = 5500
SHARD_DELAY = float(os.environ.get("SHARD_DELAY", "0"))   # 每层延时，仅用于演示/测试容错


def ws_frame(payload):
    length = len(payload)
    if length < 126:
        header = struct.pack(">BB", 0x81, length)
    elif length < 65536:
        header = struct.pack(">BBH", 0x81, 126, length)
    else:
        header = struct.pack(">BQB", 0x81, 127, length)
    return header + payload


def ws_recv_frame(conn, timeout=None):
    conn.settimeout(timeout)
    try:
        hdr = conn.recv(2)
        if len(hdr) < 2:
            return None
        opcode = hdr[0] & 0x0F
        masked = hdr[1] & 0x80
        plen = hdr[1] & 0x7F
        if plen == 126:
            plen = struct.unpack(">H", conn.recv(2))[0]
        elif plen == 127:
            plen = struct.unpack(">Q", conn.recv(8))[0]
        mask = conn.recv(4) if masked else b""
        data = b""
        while len(data) < plen:
            chunk = conn.recv(plen - len(data))
            if not chunk:
                raise ConnectionError("closed")
            data += chunk
        if masked:
            data = bytes(b ^ mask[i % 4] for i, b in enumerate(data))
        if opcode == 0x8:
            return None
        return data.decode("utf-8", "replace")
    except (socket.timeout, OSError, ConnectionError):
        return None


class Bridge:
    def __init__(self):
        self.browser = None
        self.serial = None
        self.lock = threading.Lock()
        self._shard_procs = []      # 由桥拉起的分片节点进程

    def connect_serial(self):
        while True:
            try:
                s = socket.create_connection((SERIAL_HOST, SERIAL_PORT), timeout=5)
                self.serial = s
                print("[bridge] connected to NexOS serial %s:%d" % (SERIAL_HOST, SERIAL_PORT))
                threading.Thread(target=self.read_serial, daemon=True).start()
                return
            except OSError as e:
                print("[bridge] serial not ready (%s); retry in 2s..." % e)
                time.sleep(2)

    def read_serial(self):
        try:
            self.serial.settimeout(1.0)
            last = time.time()
            while self.serial:
                try:
                    data = self.serial.recv(4096)
                except socket.timeout:
                    # idle: QEMU's tcp serial resets the link if the guest stops
                    # emitting and the tx buffer drains. Send a harmless newline
                    # to keep the shell prompt alive (and the connection open).
                    if time.time() - last > 2:
                        try:
                            self.serial.sendall(b"\n")
                            last = time.time()
                        except OSError:
                            break
                    continue
                if not data:
                    break
                last = time.time()
                text = data.decode("utf-8", "replace")
                with self.lock:
                    if self.browser:
                        try:
                            self.browser.sendall(ws_frame(text.encode("utf-8")))
                        except OSError:
                            self.browser = None
        except OSError:
            pass
        print("[bridge] serial link lost; reconnecting...")
        old = self.serial
        self.serial = None
        try:
            if old:
                old.close()
        except OSError:
            pass
        self.connect_serial()

    def send_to_serial(self, text):
        if not self.serial:
            return
        print("[ws->serial] %r" % text)
        try:
            self.serial.sendall(text.encode("utf-8"))
            if not text.endswith("\n"):
                self.serial.sendall(b"\n")
        except OSError:
            self.serial = None

    # ---- Forge 本地服务路由 ----
    def send_to_browser(self, text):
        """把本地服务(如分片推理)的输出按内核回显的格式推给浏览器。"""
        with self.lock:
            if not self.browser:
                return
            try:
                self.browser.sendall(ws_frame(text.encode("utf-8")))
            except OSError:
                self.browser = None

    def _shard_alive(self, port):
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.settimeout(0.5)
        try:
            s.sendto(b"QUERY", ("127.0.0.1", port))
            d, _ = s.recvfrom(1024)
            return d.startswith(b"BEACON")
        except OSError:
            return False
        finally:
            s.close()

    def _ensure_shard_nodes(self):
        """确保分片节点在跑;没跑就拉起,并**轮询确认真的活着**再返回。

        固定 sleep 是不行的:Python 解释器冷启动常超过 1 秒,编排器会抢在节点
        就绪前发 QUERY,表现为 `no shard nodes discovered`(其实是竞态,不是故障)。
        """
        for p in SHARD_PORTS:
            if self._shard_alive(p):
                continue
            try:
                args = [sys.executable, os.path.join(HERE, "distnet_shard_node.py"),
                        "--port", str(p), "--weight", str(SHARD_WEIGHTS[p]),
                        "--dim", "16", "--orch-port", str(SHARD_ORCH_PORT)]
                if SHARD_DELAY:
                    args += ["--delay", str(SHARD_DELAY)]
                self._shard_procs.append(subprocess.Popen(
                    args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
                print("[bridge] spawned shard node on %d" % p)
            except OSError as e:
                print("[bridge] cannot spawn shard node %d: %s" % (p, e))
        # 等待所有节点真正可应答(最多 15 秒)
        deadline = time.time() + 15
        pending = list(SHARD_PORTS)
        while pending and time.time() < deadline:
            pending = [p for p in pending if not self._shard_alive(p)]
            if pending:
                time.sleep(0.3)
        if pending:
            print("[bridge] warning: shard nodes not responding: %s" % pending)

    def handle_forge(self, cmd):
        """处理 `forge ...` 指令;认得则返回 True(不再下发内核)。"""
        t = cmd.split()
        if len(t) >= 2 and t[1] == "shard":
            layers = int(t[2]) if len(t) > 2 and t[2].isdigit() else 12
            dim = int(t[3]) if len(t) > 3 and t[3].isdigit() else 16
            threading.Thread(target=self._do_shard, args=(layers, dim), daemon=True).start()
            return True
        if len(t) >= 2 and t[1] == "shard-nodes":
            self._ensure_shard_nodes()
            self.send_to_browser("[forge] shard nodes: %s\n"
                                 % ", ".join("%d:%s" % (p, "up" if self._shard_alive(p) else "down")
                                             for p in SHARD_PORTS))
            return True
        return False

    def _do_shard(self, layers, dim):
        try:
            self.send_to_browser("[forge] dispatching sharded inference "
                                 "(layers=%d, dim=%d)...\n" % (layers, dim))
            self._ensure_shard_nodes()
            if HERE not in sys.path:
                sys.path.insert(0, HERE)
            from distnet_shard_orchestrator import Orchestrator
            nodes = [("127.0.0.1", p) for p in SHARD_PORTS]
            o = Orchestrator("127.0.0.1", SHARD_ORCH_PORT, nodes, layers, dim, 1)
            threading.Thread(target=o.serve, daemon=True).start()
            time.sleep(0.3)
            vec = [0.1 * (i + 1) for i in range(dim)]
            out, why = o.infer("forge%d" % int(time.time()), vec)
            if out is None:
                self.send_to_browser("[forge] shard inference FAILED: %s\n" % why)
            else:
                self.send_to_browser("[forge] shard inference OK: %s\n"
                                     % " ".join("%.4f" % v for v in out))
        except Exception as e:
            self.send_to_browser("[forge] error: %s\n" % e)

    def handle_ws(self, conn):
        req = b""
        conn.settimeout(5)
        try:
            while b"\r\n\r\n" not in req:
                req += conn.recv(1)
        except OSError:
            return
        key = None
        for line in req.decode("utf-8", "replace").split("\r\n"):
            if line.lower().startswith("sec-websocket-key:"):
                key = line.split(":", 1)[1].strip()
        if not key:
            conn.close()
            return
        accept = base64.b64encode(hashlib.sha1((key + GUID).encode()).digest()).decode()
        hs = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n" % accept
        conn.sendall(hs.encode())
        # Blocking from here on: we must keep reading the browser's frames for
        # the whole session. A timeout here would make us drop the connection
        # while the browser still thinks it is open (its later send() would be
        # silently lost). Only a real close (recv -> empty) ends the loop.
        conn.settimeout(None)
        print("[bridge] browser connected")
        with self.lock:
            self.browser = conn
        try:
            while True:
                msg = ws_recv_frame(conn)
                if msg is None:
                    break
                print("[ops] -> %r" % msg)
                stripped = msg.strip()
                if stripped.startswith("forge "):
                    # 本地服务(分片推理等):桥做协议翻译,不下发内核
                    if not self.handle_forge(stripped):
                        self.send_to_browser("[forge] unknown command: %s\n" % stripped)
                else:
                    self.send_to_serial(msg)
        except OSError:
            pass
        with self.lock:
            if self.browser is conn:
                self.browser = None
        print("[bridge] browser disconnected")


def main():
    bridge = Bridge()
    threading.Thread(target=bridge.connect_serial, daemon=True).start()
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((WS_HOST, WS_PORT))
    srv.listen(5)
    print("[bridge] listening on ws://%s:%d" % (WS_HOST, WS_PORT))
    try:
        while True:
            conn, _ = srv.accept()
            threading.Thread(target=bridge.handle_ws, args=(conn,), daemon=True).start()
    except KeyboardInterrupt:
        print("\n[bridge] stopped")


if __name__ == "__main__":
    main()
