#!/usr/bin/env python3
"""Host-side compute peer for the NexOS distributed compute network.

Mirrors the in-kernel compute role (distnet.cpp) but runs as a plain
Python process on the host.  This lets a QEMU SLIRP guest (acting as the
scheduler) offload a task to the host without a second VM.

Listens on UDP:
    5455  beacon   -- answers QUERY  with BEACON node0
    5456  task     -- answers TASK   with RESULT <id> <status> <value>

NOTE: these were 5355/5356/5357 originally; UDP 5355 is the LLMNR port and is
permanently held by the Windows DNS Client service (svchost), so a host peer
bound there never receives the guest's QUERY even though bind() succeeds.

Replies are sent back to the sender address returned by recvfrom(), which
is what QEMU SLIRP expects (it NATs guest<->host UDP and rewrites the
destination port the guest sees back to the guest's original source port).

Task types (same wire format as distnet.cpp):
    TASK <id> sum a b c ...   -> RESULT <id> ok <a+b+c>
    TASK <id> echo w x y      -> RESULT <id> ok w x y
    TASK <id> compute N       -> RESULT <id> ok <N*N>
    TASK <id> neg N           -> RESULT <id> ok <-N>
    TASK <id> fib N           -> RESULT <id> ok <Nth Fibonacci>
    TASK <id> prime N         -> RESULT <id> ok <#primes <= N>
    TASK <id> gcd a b         -> RESULT <id> ok <gcd(a,b)>
    TASK <id> ai <prompt...>  -> RESULT <id> ok <prompt>   (echo; no host model)

Run standalone:  python3 distnet_host_peer.py
Used by:        test_distnet.py (imports start_in_thread())
"""
import socket
import threading

BEACON_PORT = 5455
TASK_PORT   = 5456
RESULT_PORT = 5457


def _fib(n):
    a, b = 0, 1
    for _ in range(n):
        a, b = b, a + b
    return a


def _prime_count(n):
    if n < 2:
        return 0
    sieve = [True] * (n + 1)
    sieve[0] = sieve[1] = False
    for i in range(2, int(n ** 0.5) + 1):
        if sieve[i]:
            for j in range(i * i, n + 1, i):
                sieve[j] = False
    return sum(sieve)


def _gcd(a, b):
    while b:
        a, b = b, a % b
    return a


def _compute(task: bytes) -> bytes:
    parts = task.decode("latin-1", "ignore").split()
    if len(parts) < 4 or parts[0] != "TASK":
        return b"RESULT ? err bad_task"
    tid, typ = parts[1], parts[2]
    try:
        if typ == "ai":
            # No model on the host peer: echo the prompt verbatim.  Proves the
            # kernel shipped the multi-word prompt across the wire intact.
            return ("RESULT %s ok %s" % (tid, " ".join(parts[3:]))).encode()
        if typ == "sum":
            s = sum(int(x) for x in parts[3:])
            return ("RESULT %s ok %d" % (tid, s)).encode()
        if typ == "echo":
            return ("RESULT %s ok %s" % (tid, " ".join(parts[3:]))).encode()
        if typ == "compute":
            n = int(parts[3]) if len(parts) > 3 else 0
            return ("RESULT %s ok %d" % (tid, n * n)).encode()
        if typ == "neg":
            n = int(parts[3]) if len(parts) > 3 else 0
            return ("RESULT %s ok %d" % (tid, -n)).encode()
        if typ == "fib":
            n = int(parts[3]) if len(parts) > 3 else 0
            return ("RESULT %s ok %d" % (tid, _fib(n))).encode()
        if typ == "prime":
            n = int(parts[3]) if len(parts) > 3 else 0
            return ("RESULT %s ok %d" % (tid, _prime_count(n))).encode()
        if typ == "gcd":
            a = int(parts[3]) if len(parts) > 3 else 0
            b = int(parts[4]) if len(parts) > 4 else 0
            return ("RESULT %s ok %d" % (tid, _gcd(a, b))).encode()
    except Exception as e:  # noqa: BLE001
        return ("RESULT %s err %s" % (tid, e)).encode()
    return ("RESULT %s err unknown_type" % tid).encode()


def _serve_once(sock, label):
    data, addr = sock.recvfrom(4096)
    if label == "beacon" and data.strip() == b"QUERY":
        sock.sendto(b"BEACON node0", addr)
        print("[peer] beacon <- %s" % (addr,))
    elif label == "task":
        result = _compute(data)
        sock.sendto(result, addr)
        print("[peer] task %r -> %r" % (data.decode("latin-1", "ignore").strip(),
                                         result.decode()))
    return data, addr


def run(stop=None, idle=0.5):
    bs = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    bs.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    bs.bind(("0.0.0.0", BEACON_PORT))
    ts = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    ts.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    ts.bind(("0.0.0.0", TASK_PORT))
    print("[peer] listening beacon@%d task@%d" % (BEACON_PORT, TASK_PORT))
    bs.settimeout(idle)
    ts.settimeout(idle)
    while not (stop and stop.is_set()):
        for sock, label in ((bs, "beacon"), (ts, "task")):
            try:
                _serve_once(sock, label)
            except socket.timeout:
                pass
            except OSError:
                pass
    bs.close()
    ts.close()
    print("[peer] stopped")


def start_in_thread():
    """Start the peer in a daemon thread.  Returns the threading.Event that
    stops it (call .set() to shut down)."""
    stop = threading.Event()
    t = threading.Thread(target=run, args=(stop,), daemon=True)
    t.start()
    return stop


if __name__ == "__main__":
    try:
        run()
    except KeyboardInterrupt:
        print("\n[peer] bye")
