#!/usr/bin/env python3
# Resumable downloader for the Qwen2-0.5B-Instruct q4_k_m GGUF from ModelScope.
import os, sys, time, urllib.request, urllib.error

URL = "https://modelscope.cn/models/Qwen/Qwen2-0.5B-Instruct-GGUF/resolve/master/qwen2-0_5b-instruct-q4_k_m.gguf"
DST = r"d:\MyOS\bootloader\build\qwen2-0_5b-instruct-q4_k_m.gguf"
CHUNK = 1 << 20  # 1 MiB

def remote_size():
    req = urllib.request.Request(URL, method="HEAD")
    r = urllib.request.urlopen(req, timeout=30)
    return int(r.headers.get("Content-Length", "0"))

def main():
    existing = os.path.getsize(DST) if os.path.exists(DST) else 0
    total = 0
    # Probe size with a range request if HEAD lacks length
    try:
        total = remote_size()
    except Exception:
        total = 0
    if total and existing >= total:
        print(f"ALREADY_COMPLETE {existing} bytes")
        return
    start = existing
    req = urllib.request.Request(URL, headers={"Range": f"bytes={start}-"})
    r = urllib.request.urlopen(req, timeout=60)
    total = int(r.headers.get("Content-Range", f"*/{total}").split("/")[-1] or total)
    mode = "ab" if start else "wb"
    print(f"RESUME={start} TOTAL={total}")
    with open(DST, mode) as f:
        f.seek(start)
        got = start
        t0 = time.time()
        while True:
            buf = r.read(CHUNK)
            if not buf:
                break
            f.write(buf)
            got += len(buf)
            el = time.time() - t0
            spd = (got - start) / el / 1e6 if el > 0 else 0
            sys.stdout.write(f"\r{got}/{total} ({got*100//total}%) {spd:.1f} MB/s")
            sys.stdout.flush()
    print(f"\nDONE {os.path.getsize(DST)} bytes")

if __name__ == "__main__":
    main()
