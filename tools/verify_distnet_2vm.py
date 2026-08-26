#!/usr/bin/env python3
"""NexOS 分布式算力网络 — 双虚拟机一键验收脚本 (2-VM acceptance)

启动 TWO 个 QEMU guest 共享一条 L2 UDP socket 链路（无 SLIRP，走真实
广播发现路径）：

    guest A (scheduler, 10.0.2.15) : distnet scheduler / distnet ai
    guest B (compute,   10.0.2.16) : setip 10.0.2.16; distnet compute

验收项：
  1. [链路] scheduler 广播 QUERY -> compute 回 BEACON -> 派发 sum 任务
            -> RESULT 1 ok 15            (发现 + 派发 + 执行 + 回传全链路)
  2. [AI]   scheduler 派发 `distnet ai <prompt>` -> compute 端内置 AI 引擎
            (GGUF 模型若存在走真推理；无 model.gguf 走内置 Markov 引擎，
            kern_ai_ask 仍返回推理文本) -> RESULT 1 ok <文本>
  3. [节点] 任一 VM 上 `distnet nodes` 能列出对端

用法:
    python3 verify_distnet_2vm.py [path/to/os_textboot.img]

退出码: 0 = 全部 PASS, 1 = 有 FAIL
"""
import os
import re
import sys
import socket
import time
import subprocess
import shutil

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)

QEMU = os.environ.get("QEMU_BIN") or shutil.which("qemu-system-x86_64")
if not QEMU:
    for cand in ("/d/qemu/qemu-system-x86_64.exe",
                 "/c/Program Files/qemu/qemu-system-x86_64.exe",
                 "D:/qemu/qemu-system-x86_64.exe"):
        if os.path.exists(cand):
            QEMU = cand
            break
if not QEMU:
    print("ERROR: qemu-system-x86_64 not found")
    sys.exit(2)

IMG = sys.argv[1] if len(sys.argv) > 1 else "build/os_textboot.img"
SK = "127.0.0.1:1234"           # UDP socket tunnel between the two guests
MA_PORT, MB_PORT = 4481, 4482   # QEMU monitors (fresh ports)
SA_LOG = "build/serial_2vm_a.log"   # scheduler guest serial
SB_LOG = "build/serial_2vm_b.log"   # compute guest serial
COMPUTE_IP = "10.0.2.16"
PROMPT = "what is the capital of china"

# AI task prompt: use an ASCII-safe phrase so QEMU sendkey can type it.
# (sendkey has no '"' key, so no shell quotes - distnet takes rest of line.)


def wait_sock(port, timeout=30.0):
    end = time.time() + timeout
    while time.time() < end:
        try:
            return socket.create_connection(("127.0.0.1", port), timeout=0.5)
        except OSError:
            time.sleep(0.2)
    raise RuntimeError("monitor not ready: %d" % port)


def type_line(mon, s):
    keymap = {' ': 'spc', '.': 'dot', '/': 'slash', '\\': 'backslash',
              '-': 'minus', '_': 'shift-minus', '?': 'shift-slash'}
    for ch in s:
        key = "shift-%s" % ch.lower() if 'A' <= ch <= 'Z' else keymap.get(ch, ch)
        mon.sendall(("sendkey %s\n" % key).encode())
        time.sleep(0.05)
    mon.sendall(b"sendkey ret\n")
    time.sleep(0.3)


def launch(role, port, log, sockopt):
    return subprocess.Popen([
        QEMU,
        "-drive", "format=raw,file=%s" % IMG,
        "-m", "32M",
        "-vga", "std",
        "-display", "none",
        "-accel", "tcg,tb-size=8",
        "-net", "nic,model=ne2k_isa",
        "-net", sockopt,
        "-monitor", "tcp:127.0.0.1:%d,server,nowait" % port,
        "-chardev", "file,id=ser,path=%s" % log,
        "-serial", "chardev:ser",
    ], stdout=open("build/qemu_2vm_%s.err" % role, "wb"),
       stderr=subprocess.STDOUT)


def login(mon, log_path):
    deadline = time.time() + 40.0
    while time.time() < deadline:
        try:
            data = open(log_path, "rb").read().decode("latin-1", "ignore")
            if "login:" in data:
                break
        except OSError:
            pass
        time.sleep(0.5)
    type_line(mon, "root")
    type_line(mon, "admin")
    time.sleep(1.0)


def read_log(path):
    try:
        return open(path, "rb").read().decode("latin-1", "ignore")
    except OSError:
        return ""


def wait_for(log_path, needle, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if needle in read_log(log_path):
            return True
        time.sleep(0.4)
    return False


def kill(procs):
    for p in procs:
        try:
            p.terminate()
            p.wait(timeout=3.0)
        except Exception:
            try:
                p.kill()
            except Exception:
                pass


def main():
    if not os.path.exists(IMG):
        print("ERROR: image not found: %s" % IMG)
        return 1
    for f in (SA_LOG, SB_LOG):
        if os.path.exists(f):
            open(f, "w").close()

    results = []          # (name, ok, detail)

    print("=" * 66)
    print(" NexOS 分布式算力网络 — 双虚拟机验收")
    print("  scheduler A = 10.0.2.15   compute B = 10.0.2.16")
    print("=" * 66)

    procA = procB = None
    try:
        # Scheduler guest = listen end of the socket tunnel.
        procA = launch("a", MA_PORT, SA_LOG, "socket,listen=%s" % SK)
        time.sleep(2.0)   # let the listen socket come up before connect
        procB = launch("b", MB_PORT, SB_LOG, "socket,connect=%s" % SK)

        monA = wait_sock(MA_PORT)
        monB = wait_sock(MB_PORT)
        login(monA, SA_LOG)
        login(monB, SB_LOG)
        print("[1/4] 两个 VM 已启动并登录")

        # ---- compute guest: setip + distnet compute ----
        print("[2/4] compute B: setip %s ; distnet compute" % COMPUTE_IP)
        type_line(monB, "setip %s" % COMPUTE_IP)
        time.sleep(0.5)
        type_line(monB, "distnet compute")
        if not wait_for(SB_LOG, "compute node online", 20.0):
            results.append(("compute 节点上线", False, "serial 未见 'compute node online'"))
        else:
            results.append(("compute 节点上线", True, "B: beacon@5455 task@5456"))

        # ---- scheduler: sum task (full link check) ----
        print("[3/4] scheduler A: distnet scheduler  (sum 1..5 -> 15)")
        type_line(monA, "distnet scheduler")
        ok = wait_for(SA_LOG, "RESULT 1 ok 15", 60.0)
        detail = "A: RESULT 1 ok 15"
        if ok and "discovered compute node" in read_log(SA_LOG):
            detail += " ; 发现节点 " + \
                (re.search(r"discovered compute node ([\d.]+)", read_log(SA_LOG))
                 .group(1) if re.search(r"discovered compute node ([\d.]+)",
                                        read_log(SA_LOG)) else "?")
        results.append(("sum 任务全链路 (发现+派发+执行+回传)", ok, detail))

        # ---- scheduler: AI question-answer ----
        print("[4/4] scheduler A: distnet ai %s" % PROMPT)
        type_line(monA, "distnet ai %s" % PROMPT)
        # Wait for the AI-specific RESULT (the Markov engine echoes the prompt,
        # so "RESULT 1 ok what is the capital of china" is unambiguous -- the
        # earlier sum result "RESULT 1 ok 15" must NOT satisfy this).
        ai_needle = "RESULT 1 ok %s" % PROMPT
        ok = wait_for(SA_LOG, ai_needle, 90.0)
        detail = "A: AI 回答 = %r" % ai_needle
        m = re.search(r"RESULT: RESULT 1 ok (.*)", read_log(SA_LOG))
        if m and PROMPT in m.group(1):
            detail = "A: AI 回答 = %r" % m.group(1)[:70]
        results.append(("AI 问答 (distnet ai, 内置引擎)", ok, detail))

        # ---- scheduler: distnet nodes ----
        type_line(monA, "distnet nodes")
        ok = wait_for(SA_LOG, "nodes: total 1", 30.0)
        results.append(("distnet nodes 节点列表", ok, "A: nodes: total 1"))

        print("\n" + "=" * 66)
        print(" 验收报告")
        print("=" * 66)
        for name, ok, detail in results:
            print("  [%s] %s  %s" % ("PASS" if ok else "FAIL", name, detail))
        print("=" * 66)

        print("\n--- scheduler (A) serial tail ---")
        print("\n".join(read_log(SA_LOG).splitlines()[-18:]))
        print("\n--- compute (B) serial tail ---")
        print("\n".join(read_log(SB_LOG).splitlines()[-12:]))
    finally:
        kill([p for p in (procA, procB) if p])

    all_ok = all(ok for _, ok, _ in results)
    print("\nRESULT:", "PASS" if all_ok else "FAIL",
          "(%d/%d)" % (sum(1 for _, ok, _ in results if ok), len(results)))
    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(main())
