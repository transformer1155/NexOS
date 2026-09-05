import socket, time

def check(port, label):
    try:
        s = socket.create_connection(("127.0.0.1", port), 3)
        print("[%s] port %d OPEN" % (label, port))
        s.close()
        return True
    except OSError as e:
        print("[%s] port %d CLOSED: %s" % (label, port, e))
        return False

if __name__ == "__main__":
    import sys
    p = int(sys.argv[1]) if len(sys.argv) > 1 else 4321
    for i in range(6):
        check(p, "t=%ds" % (i * 2))
        time.sleep(2)
