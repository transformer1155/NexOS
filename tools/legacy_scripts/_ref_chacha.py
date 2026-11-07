import struct
M=0xFFFFFFFF
def rotl(x,n): return ((x<<n)|(x>>(32-n))&M)&M

def chacha_block(key,counter,nonce):
    assert len(key)==32 and len(nonce)==12
    s=[0x61707865,0x3320646e,0x79622d32,0x6b206574]
    s+=list(struct.unpack('<8I',key))
    s+=[counter]+list(struct.unpack('<3I',nonce))
    w=s[:]
    def qr(a,b,c,d):
        w[a]=(w[a]+w[b])&M; w[d]=rotl(w[d]^w[a],16)
        w[c]=(w[c]+w[d])&M; w[b]=rotl(w[b]^w[c],12)
        w[a]=(w[a]+w[b])&M; w[d]=rotl(w[d]^w[a],8)
        w[c]=(w[c]+w[d])&M; w[b]=rotl(w[b]^w[c],7)
    for _ in range(10):
        qr(0,4,8,12);qr(1,5,9,13);qr(2,6,10,14);qr(3,7,11,15)
        qr(0,5,10,15);qr(1,6,11,12);qr(2,7,8,13);qr(3,4,9,14)
    out=struct.pack('<16I',*[((w[i]+s[i])&M) for i in range(16)])
    return out

def chacha_xor(key,counter,nonce,data):
    out=bytearray(); i=0; c=counter
    while i<len(data):
        ks=chacha_block(key,c,nonce); c+=1
        chunk=data[i:i+64]
        out+=bytes(a^b for a,b in zip(chunk,ks))
        i+=64
    return bytes(out)

# ---- A.2 ----
key=bytes(range(0x80,0xa0))
nonce=bytes.fromhex('000000000000004a00000000')
pt=("Ladies and Gentlemen of the class of '99: If I could offer you only one "
    "tip for the future, sunscreen would be it.").encode()
ct=chacha_xor(key,1,nonce,pt)
print("A.2 pt len:",len(pt))
print("A.2 ct:",ct.hex())

# ---- Poly1305 reference (Python big int) ----
def poly1305(key,msg):
    r=int.from_bytes(key[:16],'little') & 0x0ffffffc0ffffffc0ffffffc0fffffff
    s=int.from_bytes(key[16:32],'little')
    p=(1<<130)-5
    a=0
    for i in range(0,len(msg),16):
        chunk=msg[i:i+16]
        n=int.from_bytes(chunk,'little')+(1<<(8*len(chunk)))
        a=((a+n)*r)%p
    a=(a+s)%(1<<128)
    return a.to_bytes(16,'little')

k3=bytes.fromhex('36e5f6b5c5e06070f0efca96227a863e'+'00'*16)
m="Any submission to the IETF intended by the Contributor for publication as all or part of an IETF Internet-Draft or RFC and any statement made within the context of an IETF activity is considered an \"IETF Contribution\". Such statements include oral statements in IETF sessions, as well as written and electronic communications made at any time or place, which are addressed to".encode()
print("A.3#3 poly1305:",poly1305(k3,m).hex())

# ---- A.5 AEAD tag ----
aad=bytes.fromhex('50515253c0c1c2c3c4c5c6c7')
pk=chacha_block(key,0,nonce)[:32]
def pad16(b): 
    return b + b'\x00'*((16-len(b)%16)%16)
mac_in = pad16(aad)+pad16(ct)+struct.pack('<Q',len(aad))+struct.pack('<Q',len(ct))
tag = poly1305(pk,mac_in)
print("A.5 tag:",tag.hex())
