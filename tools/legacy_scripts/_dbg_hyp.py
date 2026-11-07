import hashlib, struct
K = [0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2]
M=0xFFFFFFFF
def rr(x,n): return ((x>>n)|((x<<(32-n))&M))&M
def md_block(block):
    assert len(block)==64, len(block)
    h=[0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19]
    w=[struct.unpack('>I',block[4*i:4*i+4])[0] for i in range(16)]
    for i in range(16,64):
        s0=rr(w[i-15],7)^rr(w[i-15],18)^(w[i-15]>>3)
        s1=rr(w[i-2],17)^rr(w[i-2],19)^(w[i-2]>>10)
        w.append((w[i-16]+s0+w[i-7]+s1)&M)
    a,b,c,d,e,f,g,hh=h
    for i in range(64):
        S1=rr(e,6)^rr(e,11)^rr(e,25); ch=(e&f)^((~e&M)&g)
        t1=(hh+S1+ch+K[i]+w[i])&M
        S0=rr(a,2)^rr(a,13)^rr(a,22); maj=(a&b)^(a&c)^(b&c)
        t2=(S0+maj)&M
        hh=g;g=f;f=e;e=(d+t1)&M;d=c;c=b;b=a;a=(t1+t2)&M
    h=[(h[0]+a)&M,(h[1]+b)&M,(h[2]+c)&M,(h[3]+d)&M,(h[4]+e)&M,(h[5]+f)&M,(h[6]+g)&M,(h[7]+hh)&M]
    return ''.join('%08x'%x for x in h)
def md_multi(blocks):
    h=[0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19]
    for blk in blocks:
        hv = md_block(blk)   # chained helper below
    return None

mine = {1:'08875e3d41314bd1dc2b2db5e4f4dd046bf3ec2799403457ae6e9f4e9d393422',
        2:'220f377672b4c324361dcdf871dce821d7122e0afac92ac44dc58e4ca4fd57af',
        3:'203b1d9016060802fe5ef80436611159de1868b58d44940e3d3979eab5f4d193'}
msg='abcde'
for n in (1,2,3):
    m=msg[:n].encode()
    pad_zeros = (56-(n+1)%64)%64
    cands={}
    cands['correct']    = m+b'\x80'+b'\x00'*pad_zeros+struct.pack('>Q',n*8)
    cands['len_le']     = m+b'\x80'+b'\x00'*pad_zeros+struct.pack('<Q',n*8)
    cands['len_bits_in_bytes_field'] = m+b'\x80'+b'\x00'*pad_zeros+struct.pack('>Q',n)
    cands['len_total_after_pad']     = m+b'\x80'+b'\x00'*pad_zeros+struct.pack('>Q',(n+1+pad_zeros+8)*8)
    cands['no_len_field']= m+b'\x80'+b'\x00'*((63-n)%64)
    print("== len",n,"mine",mine[n])
    for k,v in cands.items():
        if len(v)!=64: 
            print("   %-28s (len %d, skip)" % (k,len(v))); continue
        got=md_block(v)
        if got==mine[n]: print("   %-28s MATCH  %s" % (k,got))
        else: print("   %-28s %s" % (k,got))
