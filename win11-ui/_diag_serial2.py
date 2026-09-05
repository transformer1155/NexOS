import socket, time, sys

def main():
    # QEMU is started with -serial tcp::4321,server (wait=on): it blocks until a
    # client connects, so retry-connect here until QEMU's listener is up.
    s = None
    for i in range(40):
        try:
            s = socket.create_connection(('127.0.0.1', 4321), timeout=2)
            break
        except OSError:
            time.sleep(0.5)
    if s is None:
        print('FAILED to connect to QEMU serial in 20s')
        sys.exit(1)
    s.settimeout(0.5)
    print('connected to QEMU serial (boot output follows)')
    buf = b''
    def drain(label, secs=3):
        nonlocal buf
        t0 = time.time()
        while time.time() - t0 < secs:
            try:
                d = s.recv(4096)
                if not d: break
                buf += d
            except socket.timeout:
                break
        print(f'--- {label} ---')
        print(buf.decode('latin1', 'replace').replace('\r',''))
        buf = b''

    # Drain boot output continuously (must keep reading or the tcp buffer fills
    # and QEMU resets the serial connection). Discard boot spew, keep it for debug.
    print('draining boot output for 14s...')
    t0 = time.time()
    while time.time() - t0 < 14:
        try:
            d = s.recv(65536)
            if not d: break
            buf += d
        except socket.timeout:
            continue
    print('--- boot output (first 1500 chars) ---')
    print(buf.decode('latin1','replace').replace('\r','')[:1500])
    buf = b''
    s.sendall(b'login nexos nexos\n'); drain('after login')
    s.sendall(b'distnet agent start\n'); drain('after distnet agent start')
    s.sendall(b'distnet agent status\n'); drain('after distnet agent status')
    s.sendall(b'distnet discover\n'); drain('after distnet discover')
    s.close()

if __name__ == '__main__':
    main()
