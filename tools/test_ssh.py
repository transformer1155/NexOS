#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Headless verification for the NexOS minimal SSH-2 server (net.cpp).

Boots build/os.img under QEMU with SLIRP user networking and a host forward
tcp::18022 -> guest:22, logs in as root/admin, brings up the network, then
drives a *self-contained* SSH-2 client (pure stdlib: socket + hashlib + hmac)
that speaks the exact minimal protocol the server implements:

    * banner exchange
    * kex: diffie-hellman-group14-sha1 (RFC 3526 2048-bit MODP)
    * cipher: aes128-ctr   mac: hmac-sha1   compression: none
    * auth: password (root / admin)
    * channel: session + exec "echo hello"  -> expect "hello" in output

No third-party crypto is required; AES-128-CTR and DH are implemented inline
so the test has zero external dependencies and reproduces the server's exact
(minimal, non-RFC-perfect) key-derivation so the session keys line up.

Usage:
    python tools/test_ssh.py
Exit code 0 = pass, 1 = fail.
"""
import os, sys, socket, time, subprocess, struct, hashlib, hmac

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
WORK = os.path.join(ROOT, "build", "os.img")
LOG  = os.path.join(ROOT, "build", "qemu_ssh.log")
ERR  = os.path.join(ROOT, "build", "qemu_ssh.err")
MONPORT = 18077
SSHPORT = 18042
QEMU = r"D:\qemu\qemu-system-x86_64.exe"
if not os.path.exists(QEMU):
    QEMU = "qemu-system-x86_64"

# ---------------------------------------------------------------------------
# Pure-python AES-128 (used for CTR mode).  Tiny, not fast, good enough for a
# handful of handshake + exec packets.
# ---------------------------------------------------------------------------
def _xtime(a):
    a <<= 1
    if a & 0x100:
        a ^= 0x11B
    return a & 0xFF

def _gmul(a, b):
    p = 0
    for _ in range(8):
        if b & 1:
            p ^= a
        hi = a & 0x80
        a = (a << 1) & 0xFF
        if hi:
            a ^= 0x1B
        b >>= 1
    return p & 0xFF

_SBOX = [
 0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
 0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
 0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
 0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
 0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
 0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
 0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
 0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
 0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
 0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
 0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
 0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
 0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
 0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
 0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
 0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16]

def _keyexp(key):
    rcon = [0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1B,0x36]
    w = [list(key[i:i+4]) for i in range(0,16,4)]
    for c in range(4,44):
        t = list(w[c-1])
        if c % 4 == 0:
            t = t[1:]+t[:1]
            t = [_SBOX[b] for b in t]
            t[0] ^= rcon[c//4 - 1]
        w.append([w[c-4][i] ^ t[i] for i in range(4)])
    # build round keys (11 * 16 bytes)
    rk = []
    for r in range(11):
        kb = []
        for c in range(4):
            kb += w[r*4 + c]
        rk.append(kb)
    return rk

def _addround(rk, st, r):
    for i in range(16):
        st[i] ^= rk[r][i]

def _subbytes(st):
    for i in range(16):
        st[i] = _SBOX[st[i]]

def _shiftrows(st):
    st[1],st[5],st[9],st[13] = st[5],st[9],st[13],st[1]
    st[2],st[6],st[10],st[14] = st[10],st[14],st[2],st[6]
    st[3],st[7],st[11],st[15] = st[15],st[3],st[7],st[11]

def _mixcol(st):
    for c in range(0,16,4):
        a0,a1,a2,a3 = st[c],st[c+1],st[c+2],st[c+3]
        st[c]   = _gmul(a0,2)^_gmul(a1,3)^a2^a3
        st[c+1] = a0^_gmul(a1,2)^_gmul(a2,3)^a3
        st[c+2] = a0^a1^_gmul(a2,2)^_gmul(a3,3)
        st[c+3] = _gmul(a0,3)^a1^a2^_gmul(a3,2)

def _aes_enc_block(key, blk):
    rk = _keyexp(key)
    st = list(blk)
    _addround(rk, st, 0)
    for r in range(1,10):
        _subbytes(st)
        _shiftrows(st)
        _mixcol(st)
        _addround(rk, st, r)
    _subbytes(st)
    _shiftrows(st)
    _addround(rk, st, 10)
    return bytes(st)

def aes128_ctr(key, iv, data):
    out = bytearray()
    ctr = bytearray(iv)
    off = 0
    while off < len(data):
        ks = _aes_enc_block(key, bytes(ctr))
        for i in range(min(16, len(data)-off)):
            out.append(data[off+i] ^ ks[i])
        off += 16
        for i in range(15,-1,-1):
            if (ctr[i]+1) & 0xFF:
                ctr[i] = (ctr[i]+1) & 0xFF
                break
            ctr[i] = 0
    return bytes(out)

def aes128_ctr_iv_after(key, iv, nbytes):
    """Return the IV counter state after encrypting nbytes (for advancing)."""
    ctr = bytearray(iv)
    off = 0
    while off < nbytes:
        for i in range(15,-1,-1):
            if (ctr[i]+1) & 0xFF:
                ctr[i] = (ctr[i]+1) & 0xFF
                break
            ctr[i] = 0
        off += 16
    return bytes(ctr)

# ---------------------------------------------------------------------------
# DH group14 (RFC 3526, 2048-bit MODP) -- same constant as net.cpp DH_P.
# ---------------------------------------------------------------------------
DH_P = int("FFFFFFFFFFFFFFFFC90FDAA22168C234C4C66208B9545249325B" # placeholder; replaced below
           "", 16)

# Build the prime from the exact same bytes the server uses (RFC 3526 group14).
_DH_P_BYTES = bytes([
 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFC,
 0xC9,0x90,0x0F,0xFD,0xDA,0xAA,0xA2,0x22,0x21,0x16,0x68,0x8C,0xC2,0x23,0x34,0x4C,
 0xC4,0x4C,0xC6,0x66,0x62,0x28,0x8B,0xB9,0x95,0x54,0x45,0x52,0x24,0x4A,0xAF,0xFC,
 0xC8,0x84,0x4D,0xDD,0xDC,0xC7,0x71,0x18,0x83,0x34,0x4E,0xE8,0x87,0x72,0x2A,0xAF,
 0xFF,0xFD,0xDC,0xCE,0xEB,0xB3,0x34,0x40,0x06,0x63,0x3C,0xC5,0x5E,0xE6,0x61,0x14,
 0x4B,0xB8,0x8A,0xA1,0x1C,0xC3,0x31,0x10,0x0C,0xC4,0x4D,0xDA,0xA5,0x53,0x3A,0xA9,
 0x9E,0xE9,0x9F,0xFB,0xBE,0xE6,0x67,0x78,0x8B,0xBD,0xDF,0xF1,0x1B,0xB8,0x85,0x5C,
 0xC8,0x8D,0xD3,0x32,0x2F,0xF0,0x06,0x66,0x65,0x5B,0xB5,0x52,0x2C,0xC9,0x9A,0xAB,
 0xB2,0x2E,0xE1,0x1A,0xA6,0x63,0x3E,0xE8,0x8C,0xC7,0x71,0x15,0x5A,0xA7,0x76,0x64,
 0x4B,0xB9,0x93,0x32,0x2F,0xFB,0xB9,0x9C,0xC5,0x58,0x87,0x7B,0xB8,0x82,0x2B,0xB4,
 0x4C,0xC9,0x9C,0xC8,0x8E,0xE4,0x4B,0xB4,0x4C,0xC4,0x47,0x7B,0xB9,0x95,0x50,0x09,
 0x95,0x59,0x91,0x11,0x19,0x9F,0xF0,0x04,0x46,0x6F,0xFF,0xF5,0x5D,0xDD,0xDB,0xB9,
 0x9F,0xF7,0x74,0x45,0x59,0x99,0x95,0x59,0x93,0x32,0x28,0x8C,0xC3,0x3E,0xEE,0xE9,
 0x9C,0xC1,0x1B,0xB2,0x20,0x0F,0xF2,0x2D,0xDB,0xBF,0xF8,0x8A,0xAF,0xFE,0xE0,0x05,
 0x5A,0xA6,0x6C,0xC6,0x6B,0xB5,0x50,0x09,0x9D,0xD9,0x9C,0xC7,0x7F,0xF1,0x1A,0xA3,
 0x3F,0xF8,0x8B,0xB6,0x6F,0xF5,0x5C,0xCB,0xB5,0x5B,0xBF,0xFD,0xD6,0x6C,0xC8,0x81,
])
P = int.from_bytes(_DH_P_BYTES, "big")
G = 2

# ---------------------------------------------------------------------------
# SSH message type constants (mirror net.cpp)
# ---------------------------------------------------------------------------
M_DISCONNECT=1; M_IGNORE=2; M_UNIMPLEMENTED=3; M_DEBUG=4
M_SERVICE_REQUEST=5; M_SERVICE_ACCEPT=6
M_KEXINIT=20; M_NEWKEYS=21; M_KEXDH_INIT=30; M_KEXDH_REPLY=31
M_USERAUTH_REQUEST=50; M_USERAUTH_FAILURE=51; M_USERAUTH_SUCCESS=52
M_CHANNEL_OPEN=90; M_CHANNEL_OPEN_CONFIRMATION=91; M_CHANNEL_OPEN_FAILURE=92
M_CHANNEL_DATA=94; M_CHANNEL_CLOSE=97; M_CHANNEL_REQUEST=98
M_CHANNEL_SUCCESS=100

CLIENT_BANNER = b"SSH-2.0-NexOS_ssh_test\r\n"
SERVER_BANNER = b"SSH-2.0-NexOS_0.1\r\n"

# ---------------------------------------------------------------------------
# Minimal SSH client
# ---------------------------------------------------------------------------
class SSHClient:
    def __init__(self, host, port):
        self.s = socket.create_connection((host, port), timeout=10)
        self.buf = b""
        self.client_seq = 0
        self.server_seq = 0
        self.have_enc = False
        self.c2s_iv = b""; self.c2s_key = b""
        self.s2c_iv = b""; self.s2c_key = b""
        self.session_id = None

    # -- byte stream helpers --
    def _recv_more(self):
        d = self.s.recv(4096)
        if not d:
            raise IOError("connection closed")
        self.buf += d

    def read_line(self):
        while b"\n" not in self.buf:
            self._recv_more()
        i = self.buf.index(b"\n") + 1
        line = self.buf[:i]
        self.buf = self.buf[i:]
        return line

    def read_packet(self):
        # need at least 4 bytes for length
        while len(self.buf) < 4:
            self._recv_more()
        pktlen = struct.unpack(">I", self.buf[:4])[0]
        # wait for full packet + mac(20)
        need = 4 + pktlen + (20 if self.have_enc else 0)
        while len(self.buf) < need:
            self._recv_more()
        raw = self.buf[:4+pktlen]
        mac = self.buf[4+pktlen:need] if self.have_enc else b""
        self.buf = self.buf[need:]
        if self.have_enc:
            # verify mac
            seq = struct.pack(">I", self.server_seq)
            macin = seq + raw
            exp = hmac.new(self.s2c_key, macin, hashlib.sha1).digest()
            if not hmac.compare_digest(exp, mac):
                raise IOError("HMAC mismatch")
            self.server_seq += 1
            # The wire packet is length(4, plaintext) || ct(pktlen bytes =
            # AES-CTR[ padlen(1) + payload + padding ]).  So ct[0] carries the
            # *encrypted* padding_length; decrypt the whole ct from offset 0 and
            # recover padlen from dec[0].
            ct = raw[4:]
            dec = aes128_ctr(self.s2c_key, self.s2c_iv, ct)
            self.s2c_iv = aes128_ctr_iv_after(self.s2c_key, self.s2c_iv, len(ct))
            padding = dec[0]
            payload = dec[1:]
        else:
            padding = raw[4]
            payload = raw[5:]   # skip 4 len + 1 padlen
        plen = len(payload) - padding
        return payload[:plen]

    def send_clear(self, payload):
        pad = 8 - ((len(payload)+1) % 8)
        if pad < 4: pad += 8
        pktlen = len(payload) + 1 + pad
        out = struct.pack(">I", pktlen) + bytes([pad]) + payload + os.urandom(pad)
        self.s.sendall(out)

    def send_enc(self, payload):
        pad = 16 - ((len(payload)+1) % 16)
        if pad < 4: pad += 16
        pktlen = len(payload) + 1 + pad
        pt = bytes([pad]) + payload + os.urandom(pad)
        ct = aes128_ctr(self.c2s_key, self.c2s_iv, pt)
        self.c2s_iv = aes128_ctr_iv_after(self.c2s_key, self.c2s_iv, len(pt))
        out = struct.pack(">I", pktlen) + ct
        seq = struct.pack(">I", self.client_seq)
        mac = hmac.new(self.c2s_key, seq + out, hashlib.sha1).digest()
        self.client_seq += 1
        self.s.sendall(out + mac)

    def send(self, payload):
        if self.have_enc:
            self.send_enc(payload)
        else:
            self.send_clear(payload)

    def put_str(self, b, s):
        return struct.pack(">I", len(s)) + s
    def put_cstr(self, b, s):
        return self.put_str(b, s.encode())

    # -- handshake --
    def handshake(self, user, pw):
        # banner exchange
        print("[SSH] sending client banner")
        self.s.sendall(CLIENT_BANNER)
        print("[SSH] waiting for server banner...")
        sb = self.read_line()
        print("[SSH] server banner: %r" % sb)
        if not sb.startswith(b"SSH-"):
            raise IOError("bad server banner: %r" % sb)
        # KEXINIT (client)
        print("[SSH] sending client KEXINIT")
        self.send_clear(self._build_kexinit())
        print("[SSH] waiting for server KEXINIT...")
        kb = self.read_packet()
        print("[SSH] got msg %d" % kb[0])
        if kb[0] != M_KEXINIT:
            raise IOError("expected KEXINIT, got %d" % kb[0])
        # KEXDH_INIT
        x = int.from_bytes(os.urandom(256), "big") % (P-1) + 1
        e = pow(G, x, P)
        ebytes = e.to_bytes(256, "big")
        pkt = bytes([M_KEXDH_INIT]) + self.put_str(b"", ebytes)
        self.send_clear(pkt)
        # KEXDH_REPLY
        rep = self.read_packet()
        if rep[0] != M_KEXDH_REPLY:
            raise IOError("expected KEXDH_REPLY, got %d" % rep[0])
        # parse: skip K_S (mpint), read f (mpint)
        off = 1
        ks_len = struct.unpack(">I", rep[off:off+4])[0]; off += 4 + ks_len
        f_len = struct.unpack(">I", rep[off:off+4])[0]; off += 4
        f = int.from_bytes(rep[off:off+f_len], "big")
        # compute K
        K = pow(f, x, P)
        Kbytes = K.to_bytes(256, "big")
        # H = SHA1(V_C || V_S || zeros(96) || K)  -- mirrors server (minimal)
        h = hashlib.sha1()
        h.update(CLIENT_BANNER)
        h.update(SERVER_BANNER)
        h.update(b"\x00" * 96)   # I_C + I_S + K_S placeholders
        h.update(Kbytes)
        H = h.digest()
        if self.session_id is None:
            self.session_id = H
        # derive keys (same as server)
        keyout = []
        for i in range(6):
            kbuf = Kbytes[:20] + H + bytes([0x41+i]) + self.session_id
            keyout.append(hashlib.sha1(kbuf).digest())
        self.c2s_iv  = keyout[0][:16]
        self.s2c_iv  = keyout[1][:16]
        self.c2s_key = keyout[4][:16]   # client encrypt = server c2s_key (K5)
        self.s2c_key = keyout[5][:16]   # client decrypt = server s2c_key (K6)
        # NEWKEYS
        self.send_clear(bytes([M_NEWKEYS]))
        nk = self.read_packet()
        if nk[0] != M_NEWKEYS:
            raise IOError("expected NEWKEYS, got %d" % nk[0])
        self.have_enc = True
        # SERVICE_REQUEST ssh-userauth
        pl = bytes([M_SERVICE_REQUEST]) + self.put_cstr(b"", "ssh-userauth")
        self.send(pl)
        sa = self.read_packet()
        if sa[0] != M_SERVICE_ACCEPT:
            raise IOError("expected SERVICE_ACCEPT, got %d" % sa[0])
        # USERAUTH_REQUEST password
        pl = bytes([M_USERAUTH_REQUEST]) + self.put_cstr(b"", user)
        pl += self.put_cstr(b"", "ssh-connection")
        pl += self.put_cstr(b"", "password")
        pl += bytes([0])   # FALSE (no signature)
        pl += self.put_cstr(b"", pw)
        self.send(pl)
        ar = self.read_packet()
        if ar[0] != M_USERAUTH_SUCCESS:
            raise IOError("auth failed (msg %d): %r" % (ar[0], ar[:40]))
        return True

    def _build_kexinit(self):
        b = bytes([M_KEXINIT])
        b += os.urandom(16)
        b += self.put_cstr(b"", "diffie-hellman-group14-sha1")
        b += self.put_cstr(b"", "ssh-rsa")
        b += self.put_cstr(b"", "aes128-ctr")
        b += self.put_cstr(b"", "aes128-ctr")
        b += self.put_cstr(b"", "hmac-sha1")
        b += self.put_cstr(b"", "hmac-sha1")
        b += self.put_cstr(b"", "none")
        b += self.put_cstr(b"", "none")
        b += self.put_cstr(b"", "")
        b += self.put_cstr(b"", "")
        b += bytes([0,0,0,0])
        return b

    def exec_command(self, cmd, timeout=8.0):
        # CHANNEL_OPEN session
        pl = bytes([M_CHANNEL_OPEN]) + self.put_cstr(b"", "session")
        pl += struct.pack(">I", 0)   # sender channel
        pl += struct.pack(">I", 0x400000)  # window
        pl += struct.pack(">I", 0x8000)    # max packet
        self.send(pl)
        oc = self.read_packet()
        if oc[0] != M_CHANNEL_OPEN_CONFIRMATION:
            raise IOError("channel open failed (msg %d)" % oc[0])
        # channel id is byte 1..4
        cid = struct.unpack(">I", oc[1:5])[0]
        # CHANNEL_REQUEST exec
        pl = bytes([M_CHANNEL_REQUEST]) + struct.pack(">I", cid)
        pl += self.put_cstr(b"", "exec")
        pl += bytes([1])  # want reply
        pl += self.put_cstr(b"", cmd)
        self.send(pl)
        # collect CHANNEL_DATA until close
        out = b""
        start = time.time()
        got_success = False
        got_close = False
        while time.time() - start < timeout:
            try:
                p = self.read_packet()
            except Exception:
                break
            m = p[0]
            if m == M_CHANNEL_SUCCESS:
                got_success = True
            elif m == M_CHANNEL_DATA:
                clid = struct.unpack(">I", p[1:5])[0]
                dlen = struct.unpack(">I", p[5:9])[0]
                out += p[9:9+dlen]
            elif m == M_CHANNEL_CLOSE:
                got_close = True
                break
            elif m == M_CHANNEL_REQUEST:
                pass
        return out.decode("utf-8", "ignore")

# ---------------------------------------------------------------------------
# QEMU control
# ---------------------------------------------------------------------------
def wait_sock(port, timeout=10.0):
    end = time.time() + timeout
    while time.time() < end:
        try:
            s = socket.create_connection(("127.0.0.1", port), timeout=2)
            return s
        except OSError:
            time.sleep(0.3)
    raise IOError("monitor socket not up")

def type_line(mon, text, interval=0.07):
    for ch in text:
        mon.sendall(("sendkey %s\n" % ch).encode())
        time.sleep(interval)
    mon.sendall(b"sendkey ret\n")
    time.sleep(0.2)

def main():
    fails = []
    if not os.path.exists(WORK):
        print("FAIL: %s not built" % WORK)
        return 1
    errf = open(ERR, "wb")
    qemu = subprocess.Popen([
        QEMU, "-machine", "pc",
        "-drive", "format=raw,file=%s" % WORK,
        "-m", "256M", "-accel", "tcg", "-vga", "std", "-display", "none",
        "-no-reboot",
        "-monitor", "tcp:127.0.0.1:%d,server,nowait" % MONPORT,
        "-net", "nic,model=ne2k_isa",
        "-net", "user,hostfwd=tcp::%d-:22" % SSHPORT,
        "-chardev", "file,id=ser,path=%s" % LOG,
        "-serial", "chardev:ser",
    ], stdout=errf, stderr=errf)

    try:
        mon = wait_sock(MONPORT)
        mon.settimeout(3.0)
        try:
            mon.recv(65536)
        except (TimeoutError, socket.timeout):
            pass
        time.sleep(9.0)
        # log in
        type_line(mon, "root")
        type_line(mon, "admin")
        time.sleep(1.0)
        # bring up networking (starts the TCP stack + SSH listener on :22)
        type_line(mon, "netstart")
        time.sleep(3.0)

        # wait for SSH port forward to accept
        up = False
        for _ in range(40):
            try:
                socket.create_connection(("127.0.0.1", SSHPORT), timeout=2)
                up = True
                break
            except OSError:
                time.sleep(0.3)
        if not up:
            fails.append("SSH port 22 not reachable via hostfwd")
            print("FAIL: SSH (guest:22) not reachable on localhost:%d" % SSHPORT)
        else:
            print("[SSH] guest:22 reachable on localhost:%d" % SSHPORT)
            try:
                c = SSHClient("127.0.0.1", SSHPORT)
                c.handshake("root", "admin")
                print("[SSH] KEX + password auth OK (encrypted session up)")
                out = c.exec_command("echo hello-from-ssh")
                if "hello-from-ssh" in out:
                    print("[SSH] exec 'echo hello-from-ssh' -> output: %r" % out.strip())
                else:
                    fails.append("exec output missing payload: %r" % out)
                    print("FAIL: exec output missing expected string: %r" % out)
                # a second command to confirm the channel/session persists
                out2 = c.exec_command("ver")
                if out2.strip():
                    print("[SSH] exec 'ver' -> %d bytes returned" % len(out2))
                else:
                    fails.append("ver returned no output")
                    print("FAIL: 'ver' returned no output")
            except Exception as e:
                import traceback as _tb
                _tb.print_exc()
                fails.append("SSH session error: %s" % e)
                print("FAIL: SSH session error: %s" % e)
                # dump kernel SSH diagnostic marker (net.cpp writes 0x5100)
                try:
                    mon.sendall(b"\n")
                    time.sleep(0.2)
                    mon.sendall(b"xp /1dw 0x5100\n")
                    time.sleep(0.4)
                    mon.settimeout(2)
                    try: mk = mon.recv(8192).decode(errors="replace")
                    except: mk = "(no resp)"
                    # extract the hex value line
                    import re
                    m = re.search(r'0000000000005100:\s*([0-9a-f]+)', mk)
                    print("[diag] SSH_MARK @0x5100 =", m.group(1) if m else mk.strip()[-120:])
                except Exception as ex:
                    print("[diag] marker read err: %s" % ex)
    finally:
        try:
            mon.sendall(b"quit\n")
        except Exception:
            pass
        try:
            qemu.terminate()
        except Exception:
            pass
        time.sleep(1.0)

    if fails:
        print("\n==== SSH TEST FAILED ====")
        for f in fails:
            print(" -", f)
        return 1
    print("\n==== SSH TEST PASSED ====")
    return 0

if __name__ == "__main__":
    sys.exit(main())
