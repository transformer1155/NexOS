#!/usr/bin/env python3
# nexos_l2hub.py — 让多个真实 QEMU VM 共享同一 L2 的“虚拟交换机”
#
# 为什么需要它：
#   * QEMU 点对点 `-netdev socket,listen=/connect=` 只在两端之间转发单播，
#     不洪泛 L2 广播帧 -> distnet 的 QUERY 广播到不了对端 VM。
#   * SLIRP (`-netdev user`) 不把 guest 的广播转发到宿主。
#   本 hub 让所有 VM 连到同一个中心点，并把任一端口收到的以太帧洪泛给
#   其它所有端口，从而实现真正的 L2 广播域（ARP / UDP 广播都能跨 VM）。
#
# 线格式：QEMU `-netdev socket` (非 udp/mcast) 发送的是
#         [4 字节大端长度][以太帧] 的流；本 hub 按此分帧并原样转发。
#
# 用法: python nexos_l2hub.py [host] [port]
import socket, threading, sys, struct, time

HOST = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 4322

clients = {}          # conn -> name (mac-ish label)
lock = threading.Lock()
stats = {"frames": 0, "bytes": 0}

def log(*a):
    print("[hub]", *a, flush=True)

def broadcast(frame, src):
    """把一帧（含 4 字节长度头）洪泛给除 src 外的所有客户端。"""
    pkt = struct.pack("!I", len(frame)) + frame
    with lock:
        peers = [c for c in clients if c is not src]
        for c in peers:
            try:
                c.sendall(pkt)
            except OSError:
                try: c.close()
                except Exception: pass
                clients.pop(c, None)
        stats["frames"] += 1
        stats["bytes"] += len(frame)
    if peers:
        dst_is_bcast = (len(frame) >= 6 and frame[0] == 0xFF and frame[1] == 0xFF)
        log(("BCAST" if dst_is_bcast else "UCAST"),
            "%d B -> %d peer(s) (total frames=%d)" % (len(frame), len(peers), stats["frames"]))

def handle(conn, addr):
    log("connect", addr, "(%d online)" % (len(clients) + 1))
    buf = b""
    try:
        while True:
            data = conn.recv(65536)
            if not data:
                break
            buf += data
            # 解析 [4 字节长度][帧]
            while True:
                if len(buf) < 4:
                    break
                (n,) = struct.unpack("!I", buf[:4])
                if n <= 0 or n > 65535:
                    log("bad frame length", n, "- dropping connection")
                    return
                if len(buf) < 4 + n:
                    break
                frame = buf[4:4 + n]
                buf = buf[4 + n:]
                broadcast(frame, conn)
    except OSError:
        pass
    finally:
        with lock:
            clients.pop(conn, None)
        try: conn.close()
        except Exception: pass
        log("disconnect", addr, "(%d online)" % len(clients))

def main():
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((HOST, PORT))
    srv.listen(8)
    log("L2 hub listening on %s:%d" % (HOST, PORT))
    log("VMs should use: -netdev socket,id=n0,connect=%s:%d -device ne2k_isa,mac=<unique>,..." % (HOST, PORT))
    try:
        while True:
            conn, addr = srv.accept()
            conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            with lock:
                clients[conn] = addr
            threading.Thread(target=handle, args=(conn, addr), daemon=True).start()
    except KeyboardInterrupt:
        log("shutting down")
    finally:
        srv.close()

if __name__ == "__main__":
    main()
