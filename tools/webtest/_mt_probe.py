import socket, threading, time

# Multi-threaded direct serial probe: one thread reads, another sends commands.
# If this reproduces the "only first command works" bug, the cause is QEMU chardev
# closing the connection after a burst, independent of the bridge.

def main():
    s = socket.create_connection(("127.0.0.1", 4321), 5)
    print("[mt] connected")
    s.settimeout(2)
    stop = False

    def reader():
        nonlocal stop
        buf = b""
        while not stop:
            try:
                d = s.recv(4096)
                if not d:
                    print("[mt] recv empty -> serial closed")
                    break
                buf += d
                # print incrementally
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    print("[mt-recv] " + line.decode("utf-8", "replace"))
            except socket.timeout:
                pass
            except OSError as e:
                print("[mt] recv error " + str(e))
                break

    t = threading.Thread(target=reader, daemon=True)
    t.start()

    time.sleep(3)
    for c in ["about", "ls", "users", "help"]:
        print("=== send %s ===" % c)
        try:
            s.sendall((c + "\n").encode())
        except OSError as e:
            print("[mt] send error " + str(e))
        time.sleep(1.5)
    stop = True
    time.sleep(1)
    s.close()


if __name__ == "__main__":
    main()
