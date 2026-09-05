import socket, time

def connect_once(label):
    try:
        s = socket.create_connection(("127.0.0.1", 4321), 5)
        print("[%s] CONNECTED" % label)
        s.sendall(b"about\n")
        time.sleep(1.0)
        s.settimeout(2)
        try:
            d = s.recv(4096)
            print("[%s] got %d bytes: %s" % (label, len(d), repr(d[:80])))
        except socket.timeout:
            print("[%s] no data" % label)
        s.close()
        print("[%s] closed" % label)
    except OSError as e:
        print("[%s] FAILED: %s" % (label, e))

def main():
    connect_once("conn-1")
    time.sleep(1)
    connect_once("conn-2")
    time.sleep(1)
    connect_once("conn-3")

if __name__ == "__main__":
    main()
