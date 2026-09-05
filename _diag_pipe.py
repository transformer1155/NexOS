import os, time, threading, sys

PIPE = r"\\.\pipe\nexoscom"
OUT = r"D:\MyOS\bootloader\_diag_pipe.out"
log = open(OUT, "w")
def L(*a):
    print(*a, file=log, flush=True)

fd = None
for i in range(60):
    try:
        fd = os.open(PIPE, os.O_RDWR | os.O_BINARY)
        L("[pipe] connected after %d tries" % i)
        break
    except Exception as e:
        time.sleep(1)
if fd is None:
    L("[pipe] FAILED to open pipe"); log.close(); sys.exit(1)

def reader():
    n = 0
    while True:
        try:
            d = os.read(fd, 4096)
        except Exception as e:
            L("[pipe] read err", e); break
        if not d:
            L("[pipe] EOF"); break
        n += len(d)
        L("[pipe<-%d] %r" % (n, d.decode("utf-8","replace")[:300]))
threading.Thread(target=reader, daemon=True).start()

# wait for GUI mode (~35s after qemu launch)
time.sleep(35)
for cmd in (b"whoami\n", b"login nexos nexos\n", b"ls\n", b"echo SERIAL_OK\n"):
    os.write(fd, cmd)
    L("[pipe->] sent %r" % cmd)
    time.sleep(4)
time.sleep(6)
try: os.close(fd)
except: pass
L("[pipe] done")
log.close()
