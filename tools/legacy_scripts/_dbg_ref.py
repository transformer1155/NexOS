import hashlib
out=[]
for n in range(71):
    s=''.join(chr(ord('a')+(i%26)) for i in range(n))
    out.append("%d %s"%(n,hashlib.sha256(s.encode()).hexdigest()))
open('/tmp/_ref.txt','w').write("\n".join(out)+"\n")
print("ref written")
