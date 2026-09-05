import socket, time

# Connect DIRECTLY to QEMU serial TCP (4321), bypassing the bridge.
def main():
    s = socket.create_connection(("127.0.0.1", 4321), 5)
    print("[serial] connected to 4321")
    s.settimeout(2)
    # drain any boot banner
    time.sleep(3)
    try:
        while True:
            d = s.recv(4096)
            if not d:
                break
            print("[boot] " + repr(d[:300]))
    except socket.timeout:
        print("[serial] no boot banner within 3s")

    for c in ["about", "ls", "users", "help"]:
        print("=== send to serial: %s ===" % c)
        s.sendall((c + "\n").encode())
        time.sleep(1.5)
        buf = b""
        try:
            while True:
                d = s.recv(4096)
                if not d:
                    break
                buf += d
        except socket.timeout:
            pass
        print("[resp] " + repr(buf[:400]))
        print()
    s.close()

if __name__ == "__main__":
    main()
