#!/usr/bin/env python3
"""
make_test_gguf.py - build a tiny but *real* qwen2-architecture GGUF file and a
matching reference forward pass, so gguf_infer.cpp can be validated numerically
without downloading a multi-GB model.

The file uses a mix of quantisation formats on purpose (Q4_K / Q6_K / Q8_0 /
Q4_0 / F16 / F32) so every dequantisation kernel in gguf_infer.cpp is
exercised.  The reference logits are computed from the *dequantised* weights,
i.e. exactly the values the kernel is supposed to reconstruct.

Outputs:
    build/test_model.gguf     the model
    build/test_ref.txt        reference logits + token ids for the harness
"""

import os, struct, math, random

OUT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "build")

# ---------------- model geometry ----------------
ARCH      = "qwen2"
N_LAYER   = 2
N_EMBD    = 256          # must be a multiple of 256 for K-quant rows
N_HEAD    = 4
N_KV      = 2
HEAD_DIM  = N_EMBD // N_HEAD      # 64
KV_DIM    = N_KV * HEAD_DIM       # 128
N_FF      = 512
N_VOCAB   = 320
N_CTX     = 128
ROPE_BASE = 10000.0
RMS_EPS   = 1e-6

# ---------------- ggml type ids ----------------
F32, F16, Q4_0, Q4_1, Q5_0, Q8_0 = 0, 1, 2, 3, 4, 6
Q4_K, Q5_K, Q6_K, BF16           = 10, 11, 12, 29

# ---------------- fp16 <-> fp32 ----------------
def f32_to_f16(x):
    b = struct.unpack("<I", struct.pack("<f", x))[0]
    s = (b >> 16) & 0x8000
    e = ((b >> 23) & 0xFF) - 127
    m = b & 0x7FFFFF
    if e > 15:
        return s | 0x7C00
    if e < -14:
        if e < -24:
            return s
        m |= 0x800000
        shift = -14 - e
        m >>= shift
        return s | ((m + 0x1000) >> 13)
    return s | ((e + 15) << 10) | ((m + 0x1000) >> 13)

def f16_to_f32(h):
    s = (h >> 15) & 1
    e = (h >> 10) & 0x1F
    m = h & 0x3FF
    if e == 0:
        v = m * 2.0 ** -24
    elif e == 31:
        v = float("inf") if m == 0 else float("nan")
    else:
        v = (1.0 + m / 1024.0) * 2.0 ** (e - 15)
    return -v if s else v

def rt_f16(x):
    """value after a float32 -> float16 -> float32 round trip"""
    return f16_to_f32(f32_to_f16(x))

# ---------------- quantisers (mirror llama.cpp reference kernels) -------------
def quant_q4_0(row):
    out = bytearray(); deq = []
    for i in range(0, len(row), 32):
        blk = row[i:i+32]
        amax, mval = 0.0, 0.0
        for v in blk:
            if abs(v) > amax:
                amax, mval = abs(v), v
        d = mval / -8.0 if mval != 0 else 0.0
        dh = rt_f16(d)
        idd = 1.0 / dh if dh else 0.0
        qs = [min(15, max(0, int(round(v * idd)) + 8)) for v in blk]
        out += struct.pack("<H", f32_to_f16(d))
        for j in range(16):
            out.append(qs[j] | (qs[j + 16] << 4))
        deq += [dh * (qs[j] - 8) for j in range(32)]
    return bytes(out), deq

def quant_q4_1(row):
    """32 weights / 20 B: d, m (fp16) + 16 nibble bytes, w = d*q + m."""
    out = bytearray(); deq = []
    for i in range(0, len(row), 32):
        blk = row[i:i+32]
        mn, mx = min(blk), max(blk)
        d = (mx - mn) / 15.0
        dh, mh = rt_f16(d), rt_f16(mn)
        idd = 1.0 / dh if dh else 0.0
        qs = [min(15, max(0, int(round((v - mh) * idd)))) for v in blk]
        out += struct.pack("<H", f32_to_f16(d))
        out += struct.pack("<H", f32_to_f16(mn))
        for j in range(16):
            out.append(qs[j] | (qs[j + 16] << 4))
        deq += [dh * qs[j] + mh for j in range(32)]
    return bytes(out), deq

def quant_q5_0(row):
    """32 weights / 22 B: d (fp16) + 32-bit high-bit map + 16 nibble bytes,
    w = d*(q-16) with q in 0..31."""
    out = bytearray(); deq = []
    for i in range(0, len(row), 32):
        blk = row[i:i+32]
        amax, mval = 0.0, 0.0
        for v in blk:
            if abs(v) > amax:
                amax, mval = abs(v), v
        d = mval / -16.0 if mval != 0 else 0.0
        dh = rt_f16(d)
        idd = 1.0 / dh if dh else 0.0
        qs = [min(31, max(0, int(round(v * idd)) + 16)) for v in blk]
        qh = 0
        for j in range(32):
            if qs[j] & 0x10:
                qh |= 1 << j
        out += struct.pack("<H", f32_to_f16(d))
        out += struct.pack("<I", qh)
        for j in range(16):
            out.append((qs[j] & 0xF) | ((qs[j + 16] & 0xF) << 4))
        deq += [dh * (qs[j] - 16) for j in range(32)]
    return bytes(out), deq

def quant_bf16(row):
    out = bytearray(); deq = []
    for v in row:
        b = struct.unpack("<I", struct.pack("<f", v))[0]
        hi = (b >> 16) & 0xFFFF          # truncate toward zero, like ggml
        out += struct.pack("<H", hi)
        deq.append(struct.unpack("<f", struct.pack("<I", hi << 16))[0])
    return bytes(out), deq

def quant_q8_0(row):
    out = bytearray(); deq = []
    for i in range(0, len(row), 32):
        blk = row[i:i+32]
        amax = max(abs(v) for v in blk)
        d = amax / 127.0
        dh = rt_f16(d)
        idd = 1.0 / dh if dh else 0.0
        qs = [max(-128, min(127, int(round(v * idd)))) for v in blk]
        out += struct.pack("<H", f32_to_f16(d))
        out += bytes((q & 0xFF) for q in qs)
        deq += [dh * q for q in qs]
    return bytes(out), deq

def _pack_scales_k4(ds, ms):
    """pack 8 6-bit scales + 8 6-bit mins into 12 bytes (get_scale_min_k4 layout)"""
    sc = bytearray(12)
    for j in range(4):
        sc[j]     = ds[j] & 63
        sc[j + 4] = ms[j] & 63
    for j in range(4, 8):
        sc[j + 4] = (ds[j] & 0xF) | ((ms[j] & 0xF) << 4)
        sc[j - 4] |= ((ds[j] >> 4) & 3) << 6
        sc[j]     |= ((ms[j] >> 4) & 3) << 6
    return bytes(sc)

def quant_q4_K(row):
    out = bytearray(); deq = []
    for base in range(0, len(row), 256):
        sup = row[base:base+256]
        scales, mins = [], []
        for j in range(8):
            blk = sup[j*32:(j+1)*32]
            mn = min(min(blk), 0.0)
            mx = max(blk)
            sc = (mx - mn) / 15.0
            if sc <= 0:
                sc = 0.0
            scales.append(sc)
            mins.append(-mn)
        max_sc = max(scales) if max(scales) > 0 else 0.0
        max_mn = max(mins)   if max(mins)   > 0 else 0.0
        d    = max_sc / 63.0
        dmin = max_mn / 63.0
        dh, dmh = rt_f16(d), rt_f16(dmin)
        qsc = [min(63, int(round(s / dh)))  if dh  else 0 for s in scales]
        qmn = [min(63, int(round(m / dmh))) if dmh else 0 for m in mins]
        qs = bytearray()
        for j in range(8):
            d1 = dh * qsc[j]
            m1 = dmh * qmn[j]
            blk = sup[j*32:(j+1)*32]
            q = [min(15, max(0, int(round((v + m1) / d1)))) if d1 else 0 for v in blk]
            deq += [d1 * qq - m1 for qq in q]
            if j % 2 == 0:
                pending = q          # low nibbles of this 64-wide group
            else:
                for l in range(32):
                    qs.append(pending[l] | (q[l] << 4))
        out += struct.pack("<H", f32_to_f16(d))
        out += struct.pack("<H", f32_to_f16(dmin))
        out += _pack_scales_k4(qsc, qmn)
        out += bytes(qs)
    return bytes(out), deq

def quant_q5_K(row):
    """256 weights / 176 B: d, dmin, 12 B packed 6-bit scales+mins,
    32 B high bits, 128 B nibbles.  Same super-block scheme as Q4_K but the
    quants are 5 bits (0..31), the 5th bit living in the qh bitmap."""
    out = bytearray(); deq = []
    for base in range(0, len(row), 256):
        sup = row[base:base+256]
        scales, mins = [], []
        for j in range(8):
            blk = sup[j*32:(j+1)*32]
            mn = min(min(blk), 0.0)
            mx = max(blk)
            sc = (mx - mn) / 31.0
            if sc <= 0:
                sc = 0.0
            scales.append(sc)
            mins.append(-mn)
        max_sc = max(scales) if max(scales) > 0 else 0.0
        max_mn = max(mins)   if max(mins)   > 0 else 0.0
        d    = max_sc / 63.0
        dmin = max_mn / 63.0
        dh, dmh = rt_f16(d), rt_f16(dmin)
        qsc = [min(63, int(round(s / dh)))  if dh  else 0 for s in scales]
        qmn = [min(63, int(round(m / dmh))) if dmh else 0 for m in mins]
        qs = bytearray(128)
        qh = bytearray(32)
        for j in range(8):
            d1 = dh * qsc[j]
            m1 = dmh * qmn[j]
            blk = sup[j*32:(j+1)*32]
            q = [min(31, max(0, int(round((v + m1) / d1)))) if d1 else 0 for v in blk]
            deq += [d1 * qq - m1 for qq in q]
            g = j // 2                      # 64-wide group index (0..3)
            bit = 1 << (2 * g + (j & 1))    # u1 / u2 in the kernel's loop
            for l in range(32):
                if j & 1:
                    qs[g*32 + l] |= (q[l] & 0xF) << 4
                else:
                    qs[g*32 + l] |= (q[l] & 0xF)
                if q[l] & 0x10:
                    qh[l] |= bit
        out += struct.pack("<H", f32_to_f16(d))
        out += struct.pack("<H", f32_to_f16(dmin))
        out += _pack_scales_k4(qsc, qmn)
        out += bytes(qh)
        out += bytes(qs)
    return bytes(out), deq

def quant_q6_K(row):
    out = bytearray(); deq = []
    for base in range(0, len(row), 256):
        sup = row[base:base+256]
        scales = []
        for j in range(16):
            blk = sup[j*16:(j+1)*16]
            amax, mval = 0.0, 0.0
            for v in blk:
                if abs(v) > amax:
                    amax, mval = abs(v), v
            scales.append(mval / -32.0 if mval != 0 else 0.0)
        max_abs_scale = max(abs(s) for s in scales)
        if max_abs_scale == 0:
            out += bytes(208) + struct.pack("<H", 0)
            deq += [0.0] * 256
            continue
        # pick d so that the int8 scales use the full range
        iscale = -128.0 / max(scales, key=abs)
        d = 1.0 / iscale
        dh = rt_f16(d)
        qsc = [max(-128, min(127, int(round(iscale * s)))) for s in scales]
        q = [0] * 256
        vals = [0.0] * 256
        for j in range(16):
            step = dh * qsc[j]
            for l in range(16):
                idx = j * 16 + l
                qq = int(round(sup[idx] / step)) if step else 0
                qq = max(-32, min(31, qq))
                q[idx] = qq
                vals[idx] = step * qq
        ql = bytearray(128); qh = bytearray(64)
        for seg in range(2):                    # two 128-wide halves
            o = seg * 128
            for l in range(32):
                v0 = q[o + l]        + 32
                v1 = q[o + l + 32]   + 32
                v2 = q[o + l + 64]   + 32
                v3 = q[o + l + 96]   + 32
                ql[seg*64 + l]      = (v0 & 0xF) | ((v2 & 0xF) << 4)
                ql[seg*64 + l + 32] = (v1 & 0xF) | ((v3 & 0xF) << 4)
                qh[seg*32 + l] = ((v0 >> 4) & 3) | (((v1 >> 4) & 3) << 2) | \
                                 (((v2 >> 4) & 3) << 4) | (((v3 >> 4) & 3) << 6)
        out += bytes(ql) + bytes(qh)
        out += bytes((s & 0xFF) for s in qsc)
        out += struct.pack("<H", f32_to_f16(d))
        deq += vals
    return bytes(out), deq

def quant_f16(row):
    out = bytearray(); deq = []
    for v in row:
        out += struct.pack("<H", f32_to_f16(v))
        deq.append(rt_f16(v))
    return bytes(out), deq

def quant_f32(row):
    return struct.pack("<%df" % len(row), *row), [struct.unpack("<f", struct.pack("<f", v))[0] for v in row]

QUANT = {Q4_0: quant_q4_0, Q4_1: quant_q4_1, Q5_0: quant_q5_0,
         Q8_0: quant_q8_0, Q4_K: quant_q4_K, Q5_K: quant_q5_K,
         Q6_K: quant_q6_K, F16: quant_f16, BF16: quant_bf16, F32: quant_f32}

def quantise(mat, rowlen, qtype):
    """mat: flat list, row-major with `rowlen` elements per row"""
    raw = bytearray(); deq = []
    for i in range(0, len(mat), rowlen):
        b, d = QUANT[qtype](mat[i:i+rowlen])
        raw += b; deq += d
    return bytes(raw), deq

# ---------------- GGUF writer ----------------
class GGUFWriter:
    def __init__(self):
        self.kv = bytearray(); self.nkv = 0
        self.tensors = []          # (name, dims, type, data, deq)

    @staticmethod
    def _str(s):
        b = s.encode("utf-8")
        return struct.pack("<Q", len(b)) + b

    def _kv(self, key, payload):
        self.kv += self._str(key) + payload
        self.nkv += 1

    def u32(self, k, v):  self._kv(k, struct.pack("<II", 4, v))
    def f32(self, k, v):  self._kv(k, struct.pack("<I", 6) + struct.pack("<f", v))
    def string(self, k, v): self._kv(k, struct.pack("<I", 8) + self._str(v))
    def str_array(self, k, arr):
        p = struct.pack("<IIQ", 9, 8, len(arr))
        for s in arr:
            p += self._str(s)
        self._kv(k, p)

    def tensor(self, name, dims, qtype, values):
        raw, deq = quantise(values, dims[0], qtype)
        self.tensors.append((name, dims, qtype, raw, deq))

    def write(self, path, alignment=32):
        hdr = bytearray()
        hdr += b"GGUF" + struct.pack("<I", 3)
        hdr += struct.pack("<QQ", len(self.tensors), self.nkv)
        hdr += self.kv
        off = 0
        infos = bytearray()
        for name, dims, qtype, raw, _ in self.tensors:
            infos += self._str(name)
            infos += struct.pack("<I", len(dims))
            for d in dims:
                infos += struct.pack("<Q", d)
            infos += struct.pack("<I", qtype)
            infos += struct.pack("<Q", off)
            off += len(raw)
            off = (off + alignment - 1) & ~(alignment - 1)
        body = bytearray(hdr) + infos
        pad = (-len(body)) % alignment
        body += b"\x00" * pad
        for _, _, _, raw, _ in self.tensors:
            body += raw
            body += b"\x00" * ((-len(raw)) % alignment)
        with open(path, "wb") as f:
            f.write(body)
        return len(body)

# ---------------- reference forward (pure python) ----------------
def rmsnorm(x, w, eps):
    ss = sum(v * v for v in x) / len(x) + eps
    inv = 1.0 / math.sqrt(ss)
    return [w[i] * x[i] * inv for i in range(len(x))]

def matvec(w, x, n_in, n_out):
    out = [0.0] * n_out
    for j in range(n_out):
        base = j * n_in
        s = 0.0
        for i in range(n_in):
            s += w[base + i] * x[i]
        out[j] = s
    return out

def rope(vec, nheads, hd, pos, base):
    half = hd // 2
    for i in range(half):
        freq = base ** (-(2.0 * i) / hd)
        ang = pos * freq
        c, s = math.cos(ang), math.sin(ang)
        for h in range(nheads):
            o = h * hd
            x0, x1 = vec[o + i], vec[o + i + half]
            vec[o + i]        = x0 * c - x1 * s
            vec[o + i + half] = x0 * s + x1 * c

def softmax(v):
    m = max(v)
    e = [math.exp(x - m) for x in v]
    s = sum(e)
    return [x / s for x in e]

def reference_forward(W, tokens):
    kcache = [[] for _ in range(N_LAYER)]
    vcache = [[] for _ in range(N_LAYER)]
    logits = None
    for pos, tok in enumerate(tokens):
        x = W["token_embd"][tok * N_EMBD:(tok + 1) * N_EMBD][:]
        for l in range(N_LAYER):
            xb = rmsnorm(x, W["attn_norm"][l], RMS_EPS)
            q = matvec(W["wq"][l], xb, N_EMBD, N_EMBD)
            k = matvec(W["wk"][l], xb, N_EMBD, KV_DIM)
            v = matvec(W["wv"][l], xb, N_EMBD, KV_DIM)
            q = [q[i] + W["bq"][l][i] for i in range(N_EMBD)]
            k = [k[i] + W["bk"][l][i] for i in range(KV_DIM)]
            v = [v[i] + W["bv"][l][i] for i in range(KV_DIM)]
            rope(q, N_HEAD, HEAD_DIM, pos, ROPE_BASE)
            rope(k, N_KV,   HEAD_DIM, pos, ROPE_BASE)
            kcache[l].append(k); vcache[l].append(v)
            att_out = [0.0] * N_EMBD
            rep = N_HEAD // N_KV
            scale = 1.0 / math.sqrt(HEAD_DIM)
            for h in range(N_HEAD):
                kvh = h // rep
                qh = q[h*HEAD_DIM:(h+1)*HEAD_DIM]
                scores = []
                for t in range(pos + 1):
                    kk = kcache[l][t][kvh*HEAD_DIM:(kvh+1)*HEAD_DIM]
                    scores.append(sum(qh[i] * kk[i] for i in range(HEAD_DIM)) * scale)
                p = softmax(scores)
                for t in range(pos + 1):
                    vv = vcache[l][t][kvh*HEAD_DIM:(kvh+1)*HEAD_DIM]
                    a = p[t]
                    for i in range(HEAD_DIM):
                        att_out[h*HEAD_DIM + i] += a * vv[i]
            proj = matvec(W["wo"][l], att_out, N_EMBD, N_EMBD)
            x = [x[i] + proj[i] for i in range(N_EMBD)]
            xb = rmsnorm(x, W["ffn_norm"][l], RMS_EPS)
            up   = matvec(W["wu"][l], xb, N_EMBD, N_FF)
            gate = matvec(W["wg"][l], xb, N_EMBD, N_FF)
            h2 = [(g / (1.0 + math.exp(-g))) * u for g, u in zip(gate, up)]
            down = matvec(W["wd"][l], h2, N_FF, N_EMBD)
            x = [x[i] + down[i] for i in range(N_EMBD)]
        xf = rmsnorm(x, W["out_norm"], RMS_EPS)
        logits = matvec(W["token_embd"], xf, N_EMBD, N_VOCAB)   # tied embeddings
    return logits

# ---------------- build ----------------
def main():
    random.seed(20260810)
    rnd = lambda n, s=0.08: [random.uniform(-s, s) for _ in range(n)]

    gw = GGUFWriter()
    gw.string("general.architecture", ARCH)
    gw.string("general.name", "NexOS-test-qwen2")
    gw.string("general.quantization_type", "Q4_K_M")
    gw.u32(ARCH + ".block_count", N_LAYER)
    gw.u32(ARCH + ".embedding_length", N_EMBD)
    gw.u32(ARCH + ".attention.head_count", N_HEAD)
    gw.u32(ARCH + ".attention.head_count_kv", N_KV)
    gw.u32(ARCH + ".context_length", N_CTX)
    gw.u32(ARCH + ".feed_forward_length", N_FF)
    gw.f32(ARCH + ".rope.freq_base", ROPE_BASE)
    gw.f32(ARCH + ".attention.layer_norm_rms_epsilon", RMS_EPS)

    # byte-level vocabulary + a few multi-byte merges so the tokenizer is real
    def b2u():
        direct = list(range(33, 127)) + list(range(161, 173)) + list(range(174, 256))
        out, n = {}, 0
        for b in range(256):
            if b in direct:
                out[b] = b
            else:
                out[b] = 256 + n; n += 1
        return out
    BU = b2u()
    vocab = [chr(BU[b]) for b in range(256)]
    extra = ["Ġthe", "Ġa", "Ġis", "hello", "Ġworld", "ing", "ed", "Ġof", "Ġto", "Ġand",
             "Ġin", "Ġfor", "Ġon", "er", "es", "an", "or", "at", "en", "it",
             "Ġthat", "Ġwith", "Ġas", "Ġthis", "Ġbe", "Ġby", "Ġare", "Ġfrom",
             "Ġhello", "ĠNexOS", "kernel", "Ġkernel", "AI", "ĠAI", "GGUF",
             "ĠGGUF", "model", "Ġmodel", "Ġtest", "test", "run", "Ġrun",
             "<|endoftext|>", "<|im_start|>", "<|im_end|>", "Ġ", "ĊĊ", "Ċ",
             "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "Ġ0", "Ġ1",
             "Ġyou", "Ġwhat", "Ġhow", "?"]
    vocab += extra[:N_VOCAB - 256]
    while len(vocab) < N_VOCAB:
        vocab.append("<unused%d>" % len(vocab))
    gw.str_array("tokenizer.ggml.tokens", vocab)
    gw.string("tokenizer.ggml.model", "gpt2")
    gw.u32("tokenizer.ggml.bos_token_id", 256 + extra.index("<|endoftext|>"))
    gw.u32("tokenizer.ggml.eos_token_id", 256 + extra.index("<|im_end|>"))

    gw.tensor("token_embd.weight", [N_EMBD, N_VOCAB], Q4_K, rnd(N_EMBD * N_VOCAB))
    # Layer 0 and layer 1 use different quantisations so that every
    # dot_* kernel in gguf_infer.cpp gets exercised by this one model.
    LAYER_Q = [
        {"q": Q4_K, "k": Q8_0, "v": Q4_0, "o": Q5_K, "g": Q4_K, "u": F16,  "d": Q6_K},
        {"q": Q5_K, "k": Q4_1, "v": Q5_0, "o": Q6_K, "g": Q5_K, "u": BF16, "d": Q4_K},
    ]
    for l in range(N_LAYER):
        p = "blk.%d." % l
        qt = LAYER_Q[l % len(LAYER_Q)]
        gw.tensor(p + "attn_norm.weight",   [N_EMBD],           F32,     [1.0 + v for v in rnd(N_EMBD, 0.02)])
        gw.tensor(p + "attn_q.weight",      [N_EMBD, N_EMBD],   qt["q"], rnd(N_EMBD * N_EMBD))
        gw.tensor(p + "attn_k.weight",      [N_EMBD, KV_DIM],   qt["k"], rnd(N_EMBD * KV_DIM))
        gw.tensor(p + "attn_v.weight",      [N_EMBD, KV_DIM],   qt["v"], rnd(N_EMBD * KV_DIM))
        gw.tensor(p + "attn_output.weight", [N_EMBD, N_EMBD],   qt["o"], rnd(N_EMBD * N_EMBD))
        gw.tensor(p + "attn_q.bias",        [N_EMBD],           F32,     rnd(N_EMBD, 0.01))
        gw.tensor(p + "attn_k.bias",        [KV_DIM],           F32,     rnd(KV_DIM, 0.01))
        gw.tensor(p + "attn_v.bias",        [KV_DIM],           F32,     rnd(KV_DIM, 0.01))
        gw.tensor(p + "ffn_norm.weight",    [N_EMBD],           F32,     [1.0 + v for v in rnd(N_EMBD, 0.02)])
        gw.tensor(p + "ffn_gate.weight",    [N_EMBD, N_FF],     qt["g"], rnd(N_EMBD * N_FF))
        gw.tensor(p + "ffn_up.weight",      [N_EMBD, N_FF],     qt["u"], rnd(N_EMBD * N_FF))
        gw.tensor(p + "ffn_down.weight",    [N_FF,   N_EMBD],   qt["d"], rnd(N_FF * N_EMBD))
    gw.tensor("output_norm.weight", [N_EMBD], F32, [1.0 + v for v in rnd(N_EMBD, 0.02)])
    # no output.weight -> exercises the tied-embedding path

    os.makedirs(OUT_DIR, exist_ok=True)
    path = os.path.join(OUT_DIR, "test_model.gguf")
    size = gw.write(path)
    print("wrote %s (%d bytes, %d tensors)" % (path, size, len(gw.tensors)))

    # gather the dequantised weights for the reference pass
    D = {t[0]: t[4] for t in gw.tensors}
    W = {
        "token_embd": D["token_embd.weight"],
        "out_norm":   D["output_norm.weight"],
        "attn_norm": [], "ffn_norm": [], "wq": [], "wk": [], "wv": [],
        "wo": [], "wg": [], "wu": [], "wd": [], "bq": [], "bk": [], "bv": [],
    }
    for l in range(N_LAYER):
        p = "blk.%d." % l
        W["attn_norm"].append(D[p + "attn_norm.weight"])
        W["ffn_norm"].append(D[p + "ffn_norm.weight"])
        W["wq"].append(D[p + "attn_q.weight"])
        W["wk"].append(D[p + "attn_k.weight"])
        W["wv"].append(D[p + "attn_v.weight"])
        W["wo"].append(D[p + "attn_output.weight"])
        W["wg"].append(D[p + "ffn_gate.weight"])
        W["wu"].append(D[p + "ffn_up.weight"])
        W["wd"].append(D[p + "ffn_down.weight"])
        W["bq"].append(D[p + "attn_q.bias"])
        W["bk"].append(D[p + "attn_k.bias"])
        W["bv"].append(D[p + "attn_v.bias"])

    tokens = [72, 101, 108, 108, 111]          # "Hello" as raw byte tokens
    logits = reference_forward(W, tokens)
    ref = os.path.join(OUT_DIR, "test_ref.txt")
    with open(ref, "w") as f:
        f.write("tokens %s\n" % " ".join(str(t) for t in tokens))
        f.write("vocab %d\n" % N_VOCAB)
        f.write("logits\n")
        for v in logits:
            f.write("%.8e\n" % v)
    top = sorted(range(N_VOCAB), key=lambda i: -logits[i])[:5]
    print("wrote %s" % ref)
    print("reference top-5:", [(i, round(logits[i], 5)) for i in top])

if __name__ == "__main__":
    main()
