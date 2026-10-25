#!/usr/bin/env python3
# distnet_compute_driver.py — 驱动“计算节点”真实 VM 加入分布式算力网络
#
# 该 VM 与调度器 VM 通过 L2 hub (nexos_l2hub.py) 共享同一广播域。本脚本通过串口
# TCP 连接计算节点 VM，等内核启动到提示符后登录、配置 IP、持续运行 `distnet compute`
# （内核 compute 节点在若干次 BEACON 后 idle-exit，故循环重发以保持节点在线）。
#
# 重要：TCG 下内核启动到 shell 需要几十秒。连接后必须**先等提示符再发命令**，
#       否则过早写入会让 QEMU 关闭 chardev 连接，之后该端口不再监听（单连接）。
#
# 用法: python distnet_compute_driver.py <host> <port>
import socket, sys, time

HOST = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 4323

PROMPTS = ("[SHELL] $", "NexOS>", "nexos>", "$ ")
BOOT_TIMEOUT = 180.0   # 内核启动最长等待（TCG 双 VM 很慢）


def wait_for_prompt(sock, deadline):
    """累积串口输出直到看到提示符；返回 (ok, buf)。"""
    buf = b""
    while time.time() < deadline:
        sock.settimeout(1.0)
        try:
            d = sock.recv(4096)
        except socket.timeout:
            continue
        except OSError:
            return False, buf
        if not d:
            return False, buf
        buf += d
        try:
            sys.stdout.write(d.decode(errors="replace"))
            sys.stdout.flush()
        except Exception:
            pass
        if any(p.encode() in buf for p in PROMPTS):
            return True, buf
    return False, buf


def main():
    while True:
        try:
            s = socket.create_connection((HOST, PORT), timeout=10)
        except OSError as e:
            print("[driver] connect failed, retry in 3s:", e, flush=True)
            time.sleep(3)
            continue
        print("[driver] connected to compute VM serial", HOST, PORT, flush=True)
        try:
            # 1) 静默等待内核启动到提示符（不要在连上瞬间就写）
            ok, _ = wait_for_prompt(s, time.time() + BOOT_TIMEOUT)
            if not ok:
                print("[driver] no prompt seen; reconnecting", flush=True)
                s.close()
                continue

            def send(cmd):
                s.sendall((cmd + "\n").encode())

            # 2) 登录
            send("login nexos nexos")
            wait_for_prompt(s, time.time() + 30)
            # 3) 配置 IP（与调度器同网段，L2 hub 内直连）
            send("setip 10.0.2.16")
            wait_for_prompt(s, time.time() + 30)
            # 4) 持续作为 compute 节点在线
            print("[driver] compute node going online", flush=True)
            while True:
                send("distnet compute")
                ok, _ = wait_for_prompt(s, time.time() + 30)
                if not ok:
                    break
                time.sleep(1)
        except OSError as e:
            print("[driver] serial error:", e, flush=True)
        finally:
            try:
                s.close()
            except Exception:
                pass
        print("[driver] reconnecting...", flush=True)
        time.sleep(3)


if __name__ == "__main__":
    main()
