#!/usr/bin/env python3
# Minimal QEMU HMP monitor client. Sends one command per arg, prints replies.
# Usage: qmon.py [host:port] "cmd1" "cmd2" ...
import socket, sys, time

def main():
    target = "127.0.0.1:4444"
    args = sys.argv[1:]
    if args and ":" in args[0] and args[0].split(":")[-1].isdigit():
        target = args.pop(0)
    host, port = target.split(":")
    port = int(port)
    cmds = args

    s = socket.create_connection((host, port), timeout=10)
    s.settimeout(1.5)

    def recv():
        data = b""
        try:
            while True:
                ch = s.recv(4096)
                if not ch:
                    break
                data += ch
        except socket.timeout:
            pass
        return data

    time.sleep(0.3)
    banner = recv()
    sys.stderr.write(banner.decode(errors="replace"))
    for c in cmds:
        s.sendall((c + "\n").encode())
        time.sleep(0.5)
        out = recv().decode(errors="replace")
        sys.stdout.write("=== " + c + " ===\n" + out)
    s.close()

if __name__ == "__main__":
    main()
