#!/usr/bin/env python3
"""Foundation 0 end-to-end test.

Boots the image, logs in, then:
  [1] `user`  - launches a real ring-3 process (uid 1000, root /apps/userdemo)
                which must
                  * execute in ring 3 via the unified int 0x80 ABI  -> RING3_OK
                  * get a REAL return value in eax (fd) from open()
                  * read a file inside its sandbox           -> SANDBOX_FILE_OK
                  * be denied /system/passwd (absolute)      -> DENY_ABS_OK
                  * be denied ../../system/passwd (climb)    -> DENY_DOTDOT_OK
                  * never see the file content itself     (no SECRET_LEAKED)
                  * exit cleanly back to the kernel
                  * for /home/user/notes.txt (user data, NOT a credential
                    store) get a real Y/N consent prompt, driven here with
                    actual QEMU keystrokes:
                        N -> denied                        -> PERM_DENY_OK
                        A -> allowed and remembered        -> USERDOC_CONTENT
                        (3rd try) served from the cache    -> PERM_CACHED_OK
                  * exit cleanly back to the kernel
  [2] `vfs /system/passwd` - the same path read by the KERNEL process, which
                takes the SYSTEM bypass, proving the deny above is the sandbox
                talking and not a missing file.
  [3] `linux linuxhello`   - regression: the Wine/linux_compat ring-0 path.
"""
import os, sys, socket, time, subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)

IMG = sys.argv[1] if len(sys.argv) > 1 else "build/os.img"
WORK = "build/foundation0.img"
LOG = "build/serial_foundation0.log"
PORT = 4453


def wait_sock(port, timeout=30.0):
    end = time.time() + timeout
    while time.time() < end:
        try:
            return socket.create_connection(("127.0.0.1", port), timeout=0.5)
        except OSError:
            time.sleep(0.2)
    raise RuntimeError("monitor not ready")


def type_line(mon, s):
    keymap = {' ': 'spc', '.': 'dot', '/': 'slash', '\\': 'backslash',
              '-': 'minus', '_': 'shift-minus'}
    for ch in s:
        key = f"shift-{ch.lower()}" if 'A' <= ch <= 'Z' else keymap.get(ch, ch)
        mon.sendall(f"sendkey {key}\n".encode())
        time.sleep(0.08)
    mon.sendall(b"sendkey ret\n")
    time.sleep(0.4)


def send_key(mon, key):
    """Single keypress, no Enter -- this is how the Y/N prompt is answered.
    It goes through the emulated i8042, exactly like a human pressing it;
    the kernel polls port 0x60 directly, so nothing in between can fake it."""
    mon.sendall(f"sendkey {key}\n".encode())
    time.sleep(0.15)


def main():
    subprocess.run(["cp", IMG, WORK], check=True)
    if os.path.exists(LOG):
        os.remove(LOG)
    errf = open("build/qemu_foundation0.err", "wb")
    qemu = subprocess.Popen([
        "qemu-system-x86_64",
        "-drive", f"format=raw,file={WORK}",
        "-m", "128M",
        # `-vga none` on purpose.  Since the kernel started booting straight
        # into the graphical shell (g_auto_gui), a VGA-equipped guest hands
        # the keyboard to the GUI and the text shell is never reachable --
        # every keystroke below went to the desktop and the whole suite failed
        # with "`user` never reached the shell".  With no VGA adapter,
        # g_vbe_active stays false and the kernel falls back to the text
        # shell, which is what this (serial-only) test actually exercises.
        "-vga", "none",
        "-display", "none",
        "-no-reboot",
        "-monitor", f"tcp:127.0.0.1:{PORT},server,nowait",
        "-chardev", f"file,id=ser,path={LOG}",
        "-serial", "chardev:ser",
    ], stdout=errf, stderr=errf)

    try:
        mon = wait_sock(PORT)
        mon.settimeout(3.0)
        try:
            mon.recv(65536)
        except (TimeoutError, socket.timeout):
            pass
        time.sleep(8.0)
        type_line(mon, "root")
        type_line(mon, "admin")
        time.sleep(1.0)
        print("[1] user  (ring-3 process + VFS sandbox + Y/N consent)")
        type_line(mon, "user")
        # The guest now runs to the first consent prompt. Each prompt has a
        # ~1s tapjacking guard during which input is drained, so answer late.
        time.sleep(3.0)
        print("    -> prompt 1: answering N (deny once)")
        send_key(mon, "n")
        time.sleep(3.0)
        print("    -> prompt 2: answering A (allow + remember)")
        send_key(mon, "a")
        time.sleep(3.0)
        print("    -> prompt 3 must NOT appear (served from the grant cache)")
        print("[2] vfs /system/passwd  (SYSTEM bypass)")
        type_line(mon, "vfs /system/passwd")
        time.sleep(1.5)
        # Regression check: the Wine/linux_compat path must still work.
        print("[3] linux linuxhello  (regression check)")
        type_line(mon, "linux linuxhello")
        time.sleep(2.0)
        mon.sendall(b"quit\n")
        time.sleep(1.0)
    finally:
        try:
            qemu.wait(timeout=5.0)
        except subprocess.TimeoutExpired:
            qemu.terminate()
            qemu.wait(timeout=3.0)

    errf.close()
    if not os.path.exists(LOG):
        print("ERROR: serial log was never created. QEMU stderr:")
        with open("build/qemu_foundation0.err", "rb") as f:
            print(f.read().decode("latin-1", "ignore")[:2000])
        return 1

    with open(LOG, "rb") as f:
        data = f.read().decode("latin-1", "ignore")
    lines = data.splitlines()
    print("\n--- serial log tail ---")
    print("\n".join(lines[-30:]))

    # Isolate the window in which the ring-3 process was running, so the
    # SYSTEM-bypass read in step [2] cannot be mistaken for a sandbox leak.
    ring3 = data
    if "[SHELL] $ user" in data:
        ring3 = data.split("[SHELL] $ user", 1)[1]
        for marker in ("[SHELL] $ vfs", "[SHELL] $ linux"):
            if marker in ring3:
                ring3 = ring3.split(marker, 1)[0]

    ok = True

    def need(cond, msg):
        nonlocal ok
        if not cond:
            print("FAIL:", msg)
            ok = False

    print()
    need("EXCEPTION" not in data, "kernel exception detected")
    need("[SHELL] $ user" in data, "`user` never reached the shell")

    # --- ring-3 execution + syscall ABI ---
    need("RING3_OK" in ring3, "ring-3 process did not print RING3_OK")
    need("returned from ring-3" in data, "ring-3 did not return cleanly to the kernel")

    # --- VFS: read inside the sandbox (also proves eax carries the fd) ---
    need("SANDBOX_OPEN_FAIL" not in ring3, "open() inside the sandbox failed")
    need("SANDBOX_READ_FAIL" not in ring3, "read() inside the sandbox failed")
    need("SANDBOX_FILE_OK" in ring3, "sandboxed file content was not read back")
    need("[VFS] GRANTED" in ring3, "no GRANTED decision was logged")

    # --- VFS: containment ---
    need("DENY_ABS_OK" in ring3, "absolute path escaped the sandbox")
    need("DENY_DOTDOT_OK" in ring3, "'..' climbed out of the sandbox")
    need("SANDBOX_LEAK" not in ring3, "sandbox leaked: a forbidden open() succeeded")
    need("SECRET_LEAKED" not in ring3, "sandbox leaked: ring-3 saw /system/passwd content")
    need("outside sandbox" in ring3, "no DENIED decision was logged by the VFS")

    # --- 3.3: credential store is non-negotiable (no prompt is even offered) ---
    need("BLOCKED (non-negotiable)" in ring3,
         "/system/passwd did not hit the non-negotiable hard-deny path")
    need("PROMPT" not in ring3.split("PERM_DENY_OK")[0].split("DENY_DOTDOT_OK")[0],
         "a consent prompt was offered for the credential store - it must not be")

    # --- 3.3: consent prompt actually asked, and N was honoured ---
    need("[PERM] PROMPT" in ring3, "no consent prompt was raised for /home/user/notes.txt")
    need("[PERM] USER DENIED" in ring3, "the 'N' answer was not recorded as a denial")
    need("PERM_DENY_OK" in ring3, "open() succeeded even though the user denied it")

    # --- 3.3: A = allow + remember, and the content really came through ---
    need("[PERM] USER ALLOWED" in ring3, "the 'A' answer was not recorded as an allow")
    need("(remembered)" in ring3, "'A' did not persist the decision")
    need("GRANTED-BY-USER" in ring3, "the VFS did not honour the user's consent")
    need("USERDOC_CONTENT" in ring3, "consented file content never reached the app")
    need("PERM_ALLOW_FAIL" not in ring3, "open() failed after the user allowed it")

    # --- 3.3: the third attempt must be served from the cache, not re-prompt ---
    need("[PERM] CACHED ALLOW" in ring3, "the remembered grant was not reused")
    need("PERM_CACHED_OK" in ring3, "third open() failed instead of hitting the cache")
    need(ring3.count("[PERM] PROMPT") == 2,
         f"expected exactly 2 prompts, saw {ring3.count('[PERM] PROMPT')} "
         "(a cached grant must not re-prompt)")
    need("timed out" not in ring3, "a prompt timed out - keystrokes were not delivered")

    # --- SYSTEM bypass: the kernel CAN read the same path ---
    need("SECRET_LEAKED" in data, "SYSTEM bypass failed - /system/passwd unreadable "
                                  "even for the kernel (deny above may be a false positive)")

    # --- regression ---
    need("LINUX_OK" in data, "regression - linux_compat (Wine shim) path broke")

    print("\nRESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
