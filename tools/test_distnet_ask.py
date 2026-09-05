#!/usr/bin/env python3
# test_distnet_ask.py -- live demo of the distributed-compute network:
#   * 3 QEMU guests on a real LAN (L2 UDP socket tunnels = no kernel NAT)
#   * VM-A  = scheduler  (runs `distnet ask "..."`)
#   * VM-B  = compute #1 (runs `distnet compute`, real Qwen0.5B inference)
#   * VM-C  = compute #2 (runs `distnet compute`, real Qwen0.5B inference)
#   The scheduler discovers both compute nodes, asks the SAME question to each,
#   and MERGES their answers -- demonstrating two machines' compute power
#   combined to answer the user.
#
# Usage:
#   python3 tools/test_distnet_ask.py
#   python3 tools/test_distnet_ask.py "DeepSeek is which company?"

import os, sys, socket, subprocess, time, threading, signal

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
IMG  = os.path.join(ROOT, "build", "os.img")          # 64-bit kernel + embedded Qwen0.5B
# Windows-side QEMU lives at D:\qemu (no WSL qemu available). Override with $QEMU.
_DFLT_QEMU = r"D:\qemu\qemu-system-x86_64.exe" if os.path.exists(r"D:\qemu\qemu-system-x86_64.exe") else "qemu-system-x86_64"
QEMU = os.environ.get("QEMU", _DFLT_QEMU)
TAP_BASE = 19500                                       # UDP tunnel ports

if not os.path.exists(IMG):
    sys.exit(f"[FAIL] {IMG} not found -- build with: make MODEL_GGUF=build/qwen2-0_5b-instruct-q4_k_m.gguf")

QUESTION = sys.argv[1] if len(sys.argv) > 1 else "DeepSeek is which company?"

def l2tunnel(a, b):
    # L2 UDP socket tunnel: packets from guest A (port Ta) -> guest B (port Tb)
    pa = TAP_BASE + a*10 + b
    pb = TAP_BASE + b*10 + a
    return ["-netdev","socket,id=n%d%d,udp=127.0.0.1:%d,localaddr=127.0.0.1:%d"%(a,b,pb,pa),
            "-device","ne2k_isa,netdev=n%d%d"%(a,b)]

def qemu(idx, role, extra=""):
    cmd = [QEMU, "-m","2G","-drive","file=%s,format=raw,if=ide"%IMG,
           "-serial","telnet:127.0.0.1:%d,server,nowait"%(TAP_BASE+100+idx),
           "-monitor","none","-nographic","-no-reboot"]
    # full mesh of L2 tunnels among the 3 guests
    for other in (1,2,3):
        if other != idx:
            cmd += l2tunnel(idx, other)
    if extra: cmd += extra.split()
    return cmd

def send(ser, line, delay=0.6):
    ser.sendall((line+"\n").encode())
    time.sleep(delay)

def reader(tag, sock):
    try:
        while True:
            d = sock.recv(4096)
            if not d: break
            sys.stdout.write("[%s] %s"%(tag, d.decode(errors="replace")))
            sys.stdout.flush()
    except Exception:
        pass

def open_telnet(port):
    s = socket.create_connection(("127.0.0.1", port), timeout=30)
    threading.Thread(target=reader, args=(str(port), s), daemon=True).start()
    return s

def boot(idx, role):
    print("=== launching VM-%d (%s) ==="%(idx, role))
    p = subprocess.Popen(qemu(idx, role), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(2)
    ser = open_telnet(TAP_BASE+100+idx)
    # wait for login prompt / shell
    time.sleep(8)
    return p, ser

def main():
    procs = []
    try:
        pA, sA = boot(1, "scheduler")
        pB, sB = boot(2, "compute1")
        pC, sC = boot(3, "compute2")
        procs = [pA, pB, pC]
        time.sleep(4)

        # Both compute nodes enter compute mode (real Qwen0.5B inference path)
        print(">>> compute nodes: distnet compute")
        send(sB, "distnet compute", 1.0)
        send(sC, "distnet compute", 1.0)

        # Scheduler asks the same question to BOTH nodes, then merges answers
        print(">>> scheduler: distnet ask \"%s\""%QUESTION)
        send(sA, 'distnet ask "%s"'%QUESTION, 1.0)

        print("=== merged-answer window (60s) ===")
        time.sleep(60)
    finally:
        for p in procs:
            try: p.send_signal(signal.SIGTERM)
            except Exception: pass
        print("\n[done] shut down demo VMs")

if __name__ == "__main__":
    main()
