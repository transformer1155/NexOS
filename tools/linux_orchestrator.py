#!/usr/bin/env python3
# =====================================================================
#  linux_orchestrator.py  -  Host-side WSL2-style Linux Guest orchestrator
# =====================================================================
#
#  Three channels:
#    (1) VNC proxy   0.0.0.0:15900 -> 127.0.0.1:5900 (QEMU#2 guest x11vnc)
#    (2) Serial ctrl 127.0.0.1:15901  QEMU#1 COM2 -> this script (start/stop)
#    (3) Init push   127.0.0.1:15902  QEMU#2 COM2 -> this script (init script)
#
#  SLIRP: NexOS (QEMU#1) reaches host at 10.0.2.2.
#         QEMU#2 uses separate user-net, hostfwd exposes :5900 and :15902.
# =====================================================================

import argparse
import base64
import glob
import os
import select
import socket
import subprocess
import sys
import threading
import time

# ---- defaults (override on the command line) --------------------------
DEF_CTRL_LISTEN   = "127.0.0.1:15901"   # QEMU#1 COM2 control channel (host side)
DEF_VNC_LISTEN    = "0.0.0.0:15900"      # what NexOS connects to (10.0.2.2:15900)
DEF_LINUX_VNC     = "127.0.0.1:5900"     # QEMU#2's VNC, reachable from the host
DEF_CONSOLE_LISTEN = "127.0.0.1:15903"   # QEMU#2 ttyS0 console (login shell)
DEF_INIT_LISTEN    = "127.0.0.1:15902"    # QEMU#2 2nd serial for init-push (fallback)
DEF_VNC_TIMEOUT    = 600                   # seconds to wait for upstream VNC (apk install is slow)
DEF_QEMU2         = "qemu-system-x86_64"
DEF_LINUX_CPU     = "qemu64"
DEF_LINUX_MEM     = "3072"
DEF_LINUX_SMP     = "4"
DEF_LINUX_VGA     = "std"
DEF_LINUX_DISPLAY = "none"              # sdl | gtk | none | vnc=... (default none — no host window; guest uses Xvfb)
DEF_LINUX_IMAGE   = os.environ.get("NEXOS_LINUX_IMAGE", "")
DEF_LINUX_ISO     = os.environ.get("NEXOS_LINUX_ISO", "")
DEF_LINUX_KVM     = ""                   # set to "-enable-kvm" if your host supports it
DEF_LINUX_LOG     = os.environ.get("NEXOS_LINUX_LOG", "qemu2_guest.log")
DEF_LINUX_NAME    = "NexOS-Linux"
DEF_LINUX_INIT    = "tools/boot/linux_guest_init.sh"
DEF_LINUX_KERNEL  = os.environ.get('NEXOS_LINUX_KERNEL', '')   # vmlinuz-virt (auto-exported from ISO if empty)
DEF_LINUX_INITRD  = os.environ.get('NEXOS_LINUX_INITRD', '')   # initramfs-virt (auto-exported from ISO if empty)
DEF_LINUX_APPEND  = 'console=ttyS0 modules=loop,squashfs,sd-mod,usb-storage,ata_piix,virtio_pci,virtio_blk,virtio_net,ext4 alpine_dev=cdrom:iso9660 quiet'
DEF_GUEST_ROOT_IMG = os.environ.get('NEXOS_GUEST_ROOT_IMG', '')
DEF_GUEST_PERSIST  = os.environ.get('NEXOS_GUEST_PERSIST', '')


class LinuxOrchestrator:
    def __init__(self, args):
        self.ctrl_listen   = args.ctrl_listen
        self.vnc_listen    = args.vnc_listen
        self.linux_vnc     = args.linux_vnc
        self.console_listen = args.console_listen
        self.init_listen    = args.init_listen
        self.qemu2         = args.qemu2
        self.linux_cpu     = args.linux_cpu
        self.linux_mem     = args.linux_mem
        self.linux_smp     = args.linux_smp
        self.linux_vga     = args.linux_vga
        self.linux_display = args.linux_display
        self.linux_image   = args.linux_image
        self.linux_iso     = args.linux_iso
        self.linux_kvm     = args.linux_kvm
        self.linux_log     = args.linux_log
        self.linux_name    = args.linux_name
        self.linux_init    = args.linux_init
        self.vnc_timeout   = args.vnc_timeout
        self.guest_root_img = args.guest_root_img
        self.guest_persist  = args.guest_persist
        self.linux_kernel  = args.linux_kernel
        self.linux_initrd  = args.linux_initrd
        self.linux_append  = args.linux_append
        self.qemu2_proc    = None
        self.lock          = threading.RLock()
        self.guest_up      = False
        self.stop_ev       = threading.Event()

        # Auto-discover Alpine ISO when user gave neither image nor iso.
        if not self.linux_image and not self.linux_iso:
            candidates = glob.glob(
                os.path.join(os.path.dirname(__file__), "..", "build",
                             "alpine-virt-*.iso"))
            if candidates:
                self.linux_iso = os.path.abspath(candidates[0])
                print(f"[orch] auto-discover: using linux_iso={self.linux_iso}")

        # Auto-export vmlinuz-virt / initramfs-virt from ISO if missing.
        # We look first for pre-extracted files alongside the ISO, then fall
        # back to xorriso extraction.  Neither exists -> -cdrom -boot d fallback.
        if self.linux_iso and not self.linux_kernel:
            iso_dir = os.path.dirname(self.linux_iso)
            for base_dir in [iso_dir, os.path.join(iso_dir, "alpine_x")]:
                kp = os.path.join(base_dir, "vmlinuz-virt")
                ip = os.path.join(base_dir, "initramfs-virt")
                if os.path.exists(kp) and os.path.exists(ip):
                    self.linux_kernel = os.path.abspath(kp)
                    self.linux_initrd = os.path.abspath(ip)
                    print(f"[orch] auto-discover: linux_kernel={self.linux_kernel}")
                    print(f"[orch] auto-discover: linux_initrd={self.linux_initrd}")
                    break

        if self.linux_iso and not self.linux_kernel:
            # try xorriso extraction from the ISO itself
            try:
                outdir = os.path.join(os.path.dirname(self.linux_iso), "alpine_x")
                os.makedirs(outdir, exist_ok=True)
                def _xorriso_extract(src, dst):
                    r = subprocess.run(
                        ["xorriso", "-indev", self.linux_iso,
                         "-osirrox", "on", "-extract", src, dst],
                        capture_output=True, text=True, timeout=30)
                    return r.returncode == 0
                if _xorriso_extract("/boot/vmlinuz-virt", os.path.join(outdir, "vmlinuz-virt")) and                    _xorriso_extract("/boot/initramfs-virt", os.path.join(outdir, "initramfs-virt")):
                    self.linux_kernel = os.path.join(outdir, "vmlinuz-virt")
                    self.linux_initrd = os.path.join(outdir, "initramfs-virt")
                    print(f"[orch] xorriso extracted: linux_kernel={self.linux_kernel}")
                    print(f"[orch] xorriso extracted: linux_initrd={self.linux_initrd}")
                else:
                    print("[orch] WARNING: xorriso extract failed; falling back to -cdrom -boot d",
                          file=sys.stderr)
            except (FileNotFoundError, subprocess.TimeoutExpired):
                print("[orch] WARNING: xorriso not available; falling back to -cdrom -boot d",
                      file=sys.stderr)

        if not self.guest_root_img:
            cand = os.environ.get('NEXOS_GUEST_ROOT_IMG', '') or \
                   os.path.join(os.path.dirname(__file__), '..', 'build', 'guest_root.img')
            if os.path.exists(cand):
                self.guest_root_img = os.path.abspath(cand)
                print(f"[orch] auto-discover: guest_root_img={self.guest_root_img}")
        if not self.guest_persist and self.guest_root_img:
            self.guest_persist = "1"
            print("[orch] guest_persist=1 (guest_root_img present)")

    # ---- VNC transparent proxy (15900 <-> QEMU#2 VNC) -----------------
    def _proxy_one(self, c2vnc, vnc):
        """Bidirectional byte pump between a client and the real VNC server."""
        try:
            while True:
                rdy = select.select([c2vnc, vnc], [], [], 1.0)[0]
                if not rdy:
                    continue
                if c2vnc in rdy:
                    buf = c2vnc.recv(65536)
                    if not buf:
                        break
                    vnc.sendall(buf)
                if vnc in rdy:
                    buf = vnc.recv(65536)
                    if not buf:
                        break
                    c2vnc.sendall(buf)
        except (OSError, ConnectionError):
            pass
        finally:
            try: c2vnc.close()
            except Exception: pass
            try: vnc.close()
            except Exception: pass

    def _vnc_acceptor(self):
        host, port = self.vnc_listen.split(":")
        srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind((host, int(port)))
        srv.listen(8)
        print(f"[orch] VNC proxy listening on {self.vnc_listen} -> {self.linux_vnc}")
        while not self.stop_ev.is_set():
            try:
                c2vnc, _ = srv.accept()
            except OSError:
                break
            # connect to QEMU#2's VNC
            try:
                vh, vp = self.linux_vnc.split(":")
                vnc = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                vnc.settimeout(4.0)
                vnc.connect((vh, int(vp)))
                vnc.settimeout(None)
            except (OSError, ValueError) as e:
                print(f"[orch] VNC upstream {self.linux_vnc} unreachable: {e}")
                c2vnc.close()
                continue
            threading.Thread(target=self._proxy_one, args=(c2vnc, vnc),
                            daemon=True).start()
        srv.close()

    # ---- control channel (QEMU#1 COM2 -> this script) ----------------
    def _ctrl_acceptor(self):
        host, port = self.ctrl_listen.split(":")
        srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind((host, int(port)))
        srv.listen(4)
        print(f"[orch] control channel (COM2) listening on {self.ctrl_listen}")
        while not self.stop_ev.is_set():
            try:
                cli, _ = srv.accept()
            except OSError:
                break
            threading.Thread(target=self._ctrl_session, args=(cli,),
                            daemon=True).start()
        srv.close()

    def _ctrl_send(self, cli, line):
        try:
            cli.sendall((line + "\n").encode("ascii", "replace"))
        except OSError:
            pass

    def _ctrl_session(self, cli):
        buf = b""
        print("[orch] control: kernel attached")
        while not self.stop_ev.is_set():
            try:
                cli.settimeout(1.0)
                chunk = cli.recv(256)
            except socket.timeout:
                continue
            except OSError:
                break
            if not chunk:
                break
            buf += chunk
            # process line by line (kernel sends "start\n", "stop\n", ...)
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                cmd = line.strip().decode("ascii", "replace").strip().lower()
                if not cmd:
                    continue
                print(f"[orch] control: <- kernel '{cmd}'")
                self._handle_cmd(cli, cmd)
        cli.close()

    def _handle_cmd(self, cli, cmd):
        if cmd == "start":
            if self.guest_up:
                self._ctrl_send(cli, "ready")
                return
            self._ctrl_send(cli, "busy")  # we are bringing it up
            self._launch_guest()
            # wait until the upstream VNC is reachable before declaring ready
            if self._wait_vnc_up(timeout=self.vnc_timeout):
                self.guest_up = True
                self._ctrl_send(cli, "ready")
                print("[orch] control: guest ready")
            else:
                self._ctrl_send(cli, "error")
                print("[orch] control: guest VNC did not come up", file=sys.stderr)
        elif cmd == "stop":
            self._teardown_guest()
            self.guest_up = False
            self._ctrl_send(cli, "down")
            print("[orch] control: guest torn down")
        elif cmd == "status":
            self._ctrl_send(cli, "ready" if self.guest_up else "down")
        else:
            self._ctrl_send(cli, "error")


    # ===============================================================
    # Init-script push  (host -> QEMU#2 2nd serial COM2)
    # ===============================================================
    def _init_push_acceptor(self):
        """Listen on init_listen for QEMU#2's client-serial connection."""
        host, port = self.init_listen.split(":")
        srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind((host, int(port)))
        srv.listen(1)
        srv.settimeout(60.0)
        print(f"[orch] init-push listener on {self.init_listen} (waiting for qemu)")
        while not self.stop_ev.is_set() and self.qemu2_proc is not None:
            try:
                cli, _ = srv.accept()
            except socket.timeout:
                if self.qemu2_proc.poll() is not None:
                    print("[orch] qemu2 died before init-push")
                    break
                continue
            except OSError:
                break
            self._init_push_worker(cli)
            break
        srv.close()

    def _init_push_worker(self, cli):
        """Push linux_guest_init.sh over the serial link via base64 shim."""
        try:
            cli.settimeout(5.0)
            try: cli.recv(2048)
            except socket.timeout: pass

            init_path = self.linux_init
            if not init_path or not os.path.exists(init_path):
                print(f"[orch] init script {init_path!r} not found, skipping")
                cli.close(); return

            with open(init_path, "rb") as f:
                raw = f.read()
            print(f"[orch] pushing {init_path} ({len(raw)} bytes)")
            b64 = base64.b64encode(raw).decode("ascii")
            shim = ("\n[orch] init-script received, executing...\n"
                    "printf '%s' '" + b64 + "' | base64 -d > /tmp/init.sh && "
                    "chmod +x /tmp/init.sh && bash /tmp/init.sh &\n")
            cli.sendall(shim.encode("ascii"))
            time.sleep(0.3)
        except OSError as e:
            print(f"[orch] init-push error: {e}")
        finally:
            try: cli.close()
            except Exception: pass

    # ---- nested Linux Guest (QEMU#2) lifecycle -----------------------
    def _build_qemu2_cmd(self):
        """Construct QEMU#2 cmdline.  SLIRP-constraint compliant (virtio-net-pci)."""
        extras = []
        # persistent guest root (ext4 with Xvfb/x11vnc/chromium preinstalled) — becomes /dev/vda
        if self.guest_root_img:
            extras += ["-drive", f"file={self.guest_root_img},format=raw,if=virtio"]
            print(f"[orch] guest_root_img: {self.guest_root_img}")
        if self.linux_image:
            extras += ["-drive", f"format=qcow2,file={self.linux_image},if=virtio"]
        if self.linux_kernel and self.linux_initrd:
            # direct kernel boot (ttyS0 becomes the guest console) — no bootloader
            extras += ["-kernel", self.linux_kernel,
                       "-initrd", self.linux_initrd,
                       "-append", self.linux_append]
            if self.linux_iso:
                extras += ["-cdrom", self.linux_iso]
            print(f"[orch] direct kernel boot: kernel={self.linux_kernel}")
        elif self.linux_iso:
            extras += ["-cdrom", self.linux_iso]
            if not self.linux_image:
                extras += ["-boot", "d"]
        else:
            pass  # neither kernel+initrd nor iso — will warn below
        cmd = [
            self.qemu2,
            "-name", self.linux_name,
            "-cpu",  self.linux_cpu,
            "-smp",  self.linux_smp,
            "-m",    self.linux_mem,
            "-vga",  self.linux_vga,
            "-monitor", "none",
            "-net",  "nic,model=virtio-net-pci",
            "-net",  "user,hostfwd=tcp::5900-:5900",
            "-serial", f"tcp:{self.console_listen},server=on,wait=off",
            "-display", self.linux_display,
            "-no-reboot",
        ]
        # accel: whpx (Windows Hyper-V) if no linux_kvm specified
        if self.linux_kvm:
            cmd += self.linux_kvm.split()
        else:
            cmd += ["-accel", "whpx"]
        cmd += extras
        if not self.linux_image and not self.linux_iso:
            print("[orch] WARNING: no --linux-image/--linux-iso; boots to SeaBIOS only.",
                  file=sys.stderr)
        return cmd


    # ---- Guest console (ttyS0): drive the login shell + init push --------
    def _guest_console_thread(self):
        """Connect to QEMU#2's ttyS0 TCP server, log in as root (no password),
        push linux_guest_init.sh via heredoc+base64 chunks, fire it off in
        background, and wait for '[orch-init] ALL UP' or chromium warning."""
        import socket as _sk
        import time as _t
        import re as _re

        # CSI/ANSI escape sequence stripper: \x1b[...m and \x1b[<cursor>
        _CSI = _re.compile(rb'\x1b\[[0-9;]*[A-Za-z]')
        _OSC = _re.compile(rb'\x1b\].*?(?:\x07|\x1b\\)')

        def _strip_csi(b):
            b = _CSI.sub(b'', b)
            b = _OSC.sub(b'', b)
            # also strip BEL
            b = b.replace(b'\x07', b'')
            return b

        # 1) retry connect for up to 30s
        console_sock = None
        deadline = _t.time() + 30
        while _t.time() < deadline and not self.stop_ev.is_set():
            try:
                s = _sk.socket(_sk.AF_INET, _sk.SOCK_STREAM)
                s.settimeout(3.0)
                h, p = self.console_listen.split(":")
                s.connect((h, int(p)))
                s.settimeout(None)
                console_sock = s
                print(f"[orch] guest console attached on {self.console_listen}")
                break
            except OSError:
                try: s.close()
                except Exception: pass
                _t.sleep(1.0)
        if not console_sock:
            print("[orch] guest console connect failed (30s)", file=sys.stderr)
            return

        buf = b""
        def _send(cmd):
            try:
                console_sock.sendall(cmd.encode("ascii", "replace"))
                _t.sleep(0.15)
            except OSError:
                pass

        def _wait_contains(pattern_bytes, timeout_s, strip=True):
            """Read until raw byte pattern is found (after optional CSI strip).
            Returns True on match, False on timeout/error."""
            nonlocal buf
            deadline = _t.time() + timeout_s
            while _t.time() < deadline and not self.stop_ev.is_set():
                try:
                    console_sock.settimeout(2.0)
                    chunk = console_sock.recv(4096)
                    if not chunk: return False
                    buf += chunk
                except _sk.timeout:
                    continue
                except OSError:
                    return False
                haystack = _strip_csi(buf) if strip else buf
                if pattern_bytes in haystack:
                    return True
            return False

        # Drain any welcome banner first (login prompt is at the end)
        try:
            console_sock.settimeout(0.5)
            while True:
                extra = console_sock.recv(4096)
                if not extra: break
                buf += extra
        except (_sk.timeout, OSError):
            pass

        # 2) wait for login prompt (240s)
        print("[orch] console: waiting for login: ...")
        if not _wait_contains(b"login:", 240):
            print("[orch] console: login prompt timeout 240s", file=sys.stderr)
            return

        # 3) login as root — Alpine root has NO password; expect shell prompt
        print("[orch] console: sending root\n")
        _send("root\n")
        # root with no password → directly "localhost:~#" or similar;
        # never send empty Password: response — it is NOT expected.
        if not _wait_contains(b"#", 60):
            # rare: prompt might have been '$'
            if not _wait_contains(b"$", 5):
                print("[orch] console: shell prompt not seen after root login", file=sys.stderr)
                return
        print("[orch] console: shell acquired")

        # 4) set distinctive PS1 for reliable post-login matching
        _send("export PS1='RDY> '\n")
        _wait_contains(b"RDY>", 60)  # tolerate timeout — continue anyway

        # 5) push init script
        if self.guest_persist and self.guest_persist == "1":
            # persistent root path — no apk needed, just bring up Xvfb+x11vnc
            print("[orch] using persistent guest root (chroot path)")
            persist_sh = (
                "ip link set eth0 up 2>/dev/null; udhcpc -i eth0 -q -n -t 10 >/dev/null 2>&1\n"
                "printf 'nameserver 10.0.2.3\\n' > /etc/resolv.conf\n"
                "mkdir -p /mnt/disk; mount -t ext4 /dev/vda /mnt/disk || echo MOUNT_FAIL\n"
                "chroot /mnt/disk /usr/bin/Xvfb :0 -screen 0 1024x768x24 >/tmp/xvfb.log 2>&1 &\n"
                "sleep 5\n"
                "chroot /mnt/disk /usr/bin/x11vnc -display :0 -rfbport 5900 -nopw -forever -shared -bg -o /tmp/x11vnc.log\n"
                "sleep 4\n"
                "echo [orch-init] ALL UP\n"
            )
            for line in persist_sh.strip().split("\n"):
                _send(line + "\n")
                _wait_contains(b"RDY>", 2)  # light wait between lines
        else:
            # live ISO path — push linux_guest_init.sh (will apk install)
            init_path = os.path.join(os.path.dirname(__file__), "linux_guest_init.sh")
            if not os.path.exists(init_path):
                print(f"[orch] init script {init_path!r} not found", file=sys.stderr)
                return
            with open(init_path, "rb") as f:
                raw = f.read()
            b64 = base64.b64encode(raw).decode("ascii")
            print(f"[orch] pushing init.sh ({len(raw)}B -> {len(b64)}B b64 via heredoc)")
            heredoc = "cat > /tmp/i.b64 << 'XEOF'\n" + b64 + "\nXEOF\n"
            _send(heredoc)
            _wait_contains(b"RDY>", 10)
            _send("base64 -d /tmp/i.b64 > /tmp/i.sh && chmod +x /tmp/i.sh\n")
        if not _wait_contains(b"RDY>", 10):
            print("[orch] WARNING: heredoc+base64 may have been truncated — trying chunked fallback")
            # chunked fallback — keep it in buf for next run
            _send("rm -f /tmp/i.b64 /tmp/i.sh\n")
            offset = 0
            CHUNK = 1480
            while offset < len(b64):
                _send("printf '%s' '" + b64[offset:offset + CHUNK] + "' >> /tmp/i.b64\n")
                offset += CHUNK
            _send("base64 -d /tmp/i.b64 > /tmp/i.sh && chmod +x /tmp/i.sh\n")
            _wait_contains(b"RDY>", 30)

        # 6) fire init.sh in background
        _send("nohup sh /tmp/i.sh > /tmp/init.log 2>&1 &\n")
        print("[orch] init.sh launched, waiting for completion (900s)...")

        # 7) poll until ALL UP or chromium warning
        deadline = _t.time() + 900
        last_printed = b""
        while _t.time() < deadline and not self.stop_ev.is_set():
            try:
                console_sock.settimeout(2.0)
                chunk = console_sock.recv(4096)
                if not chunk: break
                buf += chunk
            except _sk.timeout:
                continue
            except OSError:
                break
            text = _strip_csi(buf).decode("ascii", "replace", errors="ignore")
            # Print any new [orch-init] line
            for line in text.split("\n"):
                line_b = line.strip().encode("ascii", "replace")
                if b"[orch-init]" in line_b and line_b != last_printed:
                    last_printed = line_b
                    print("[orch] console:", line_b.decode("ascii", "replace"))
            if "[orch-init] ALL UP" in text:
                print("[orch] guest init complete — ALL UP")
                break
            if "[orch-init] WARNING: chromium" in text:
                print("[orch] guest init: chromium reported; treating as ready")
                break
        try: console_sock.close()
        except Exception: pass
    def _launch_guest(self):
        with self.lock:
            if self.qemu2_proc and self.qemu2_proc.poll() is None:
                return
            cmd = self._build_qemu2_cmd()
            print("[orch] launching QEMU#2:", " ".join(cmd))
            logf = open(self.linux_log, "ab", buffering=0)
            logf.write(b"\n=== QEMU#2 boot ===\n")
            self.qemu2_proc = subprocess.Popen(
                cmd, stdin=subprocess.DEVNULL, stdout=logf, stderr=logf)
            threading.Thread(target=self._guest_console_thread, daemon=True).start()
            # (ttyS1 init-push path retired — _guest_console_thread on ttyS0 handles login+init)
    def _teardown_guest(self):
        with self.lock:
            if self.qemu2_proc and self.qemu2_proc.poll() is None:
                print("[orch] stopping QEMU#2")
                self.qemu2_proc.terminate()
                try:
                    self.qemu2_proc.wait(timeout=8)
                except subprocess.TimeoutExpired:
                    self.qemu2_proc.kill()
            self.qemu2_proc = None

    def _wait_vnc_up(self, timeout=120):
        vh, vp = self.linux_vnc.split(":")
        deadline = time.time() + timeout
        while time.time() < deadline:
            if self.stop_ev.is_set():
                return False
            try:
                s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                s.settimeout(1.0)
                s.connect((vh, int(vp)))
                s.close()
                return True
            except OSError:
                time.sleep(1.0)
        return False

    # ---- main loop ----------------------------------------------------
    def run(self):
        t_vnc  = threading.Thread(target=self._vnc_acceptor,  daemon=True)
        t_ctrl = threading.Thread(target=self._ctrl_acceptor, daemon=True)
        t_vnc.start()
        t_ctrl.start()
        print("[orch] orchestrator running.")
        print("  VNC proxy :", self.vnc_listen, "->", self.linux_vnc)
        print("  ctrl chan :", self.ctrl_listen)
        print("  init push :", self.init_listen)
        print("  qemu2     :", self.qemu2)
        print("  linux-img :", self.linux_image or "(none)")
        print("  linux-init:", self.linux_init)
        print("  NexOS->VNC: 10.0.2.2:15900  COM2 0x2F8 for ctrl")
        try:
            while not self.stop_ev.is_set():
                time.sleep(0.5)
        except KeyboardInterrupt:
            print("\n[orch] shutting down")
            self.stop_ev.set()
        finally:
            self._teardown_guest()


def parse_args():
    p = argparse.ArgumentParser(description="NexOS Linux Guest orchestrator")
    p.add_argument("--ctrl-listen",  default=os.environ.get("NEXOS_CTRL_LISTEN",  DEF_CTRL_LISTEN))
    p.add_argument("--vnc-listen",   default=os.environ.get("NEXOS_VNC_LISTEN",   DEF_VNC_LISTEN))
    p.add_argument("--linux-vnc",    default=os.environ.get("NEXOS_LINUX_VNC",    DEF_LINUX_VNC))
    p.add_argument("--console-listen", default=os.environ.get("NEXOS_LINUX_CONSOLE", DEF_CONSOLE_LISTEN))
    p.add_argument("--init-listen",     default=os.environ.get("NEXOS_INIT_LISTEN",  DEF_INIT_LISTEN))
    p.add_argument("--qemu2",        default=os.environ.get("NEXOS_QEMU2",        DEF_QEMU2))
    p.add_argument("--linux-cpu",    default=DEF_LINUX_CPU)
    p.add_argument("--linux-mem",    default=os.environ.get("NEXOS_LINUX_MEM",    DEF_LINUX_MEM))
    p.add_argument("--linux-smp",    default=DEF_LINUX_SMP)
    p.add_argument("--linux-vga",    default=DEF_LINUX_VGA)
    p.add_argument("--linux-display",default=DEF_LINUX_DISPLAY,
                  help="display backend for QEMU#2 (default: none — no host window)")
    p.add_argument("--vnc-timeout",    type=int, default=DEF_VNC_TIMEOUT,
                  help="seconds to wait for upstream VNC after start (default: 600)")
    p.add_argument("--linux-image",  default=DEF_LINUX_IMAGE)
    p.add_argument("--linux-iso",    default=DEF_LINUX_ISO)
    p.add_argument("--linux-kvm",    default=DEF_LINUX_KVM,
                  help="e.g. '-enable-kvm -cpu host'")
    p.add_argument("--linux-log",    default=DEF_LINUX_LOG)
    p.add_argument("--linux-name",   default=DEF_LINUX_NAME)
    p.add_argument("--linux-init",   default=DEF_LINUX_INIT,
                  help="path to linux_guest_init.sh; pushed over 2nd serial")
    p.add_argument("--linux-kernel",  default=os.environ.get('NEXOS_LINUX_KERNEL', DEF_LINUX_KERNEL),
                  help="vmlinuz-virt path; direct kernel boot (auto-exported from ISO if empty)")
    p.add_argument("--linux-initrd",  default=os.environ.get('NEXOS_LINUX_INITRD', DEF_LINUX_INITRD))
    p.add_argument("--linux-append",  default=os.environ.get('NEXOS_LINUX_APPEND', DEF_LINUX_APPEND),
                  help="kernel cmdline (default: console=ttyS0 ... alpine_dev=cdrom:iso9660)")
    p.add_argument("--guest-root-img", default=os.environ.get('NEXOS_GUEST_ROOT_IMG', DEF_GUEST_ROOT_IMG),
                  help="persistent guest root (ext4 with Xvfb/x11vnc/chromium); auto-discovered as build/guest_root.img")
    p.add_argument("--guest-persist",  default=os.environ.get('NEXOS_GUEST_PERSIST', DEF_GUEST_PERSIST),
                  help="1=use persistent root chroot path, 0=apk install; auto=1 when guest-root-img present")
    return p.parse_args()





if __name__ == "__main__":
    orch = LinuxOrchestrator(parse_args())
    orch.run()
