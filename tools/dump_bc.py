#!/usr/bin/env python3
# dump_bc.py - tiny NBC1 disassembler for debugging skillc output.
import sys, struct
OPS=["NOP","PUSH","POP","ADD","SUB","MUL","DIV","MOD","NEG","EQ","NE","LT","GT",
     "LE","GE","AND","OR","NOT","LOAD_L","STORE_L","LOAD_G","STORE_G","JMP","JZ",
     "JNZ","CALL","RET","HALT","DUP","LOAD_MEM","STORE_MEM","LOAD_IDX","STORE_IDX","SHL","SHR"]
NOARG={0,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,27,28,31,32,33,34}
def main():
    bc=open(sys.argv[1],'rb').read()
    assert bc[:4]==b'NBC1'
    ng,nf=struct.unpack_from('<II',bc,8)
    p=16
    print("nglobals=%d nfuncs=%d"%(ng,nf))
    funcs=[]
    for i in range(nf):
        nm=bc[p:p+20].split(b'\x00')[0].decode('latin-1'); p+=20
        e,pa,lo=struct.unpack_from('<iii',bc,p); p+=12
        funcs.append((nm,e,pa,lo)); print("func %d %-12s entry=%d params=%d locals=%d"%(i,nm,e,pa,lo))
    code_len,=struct.unpack_from('<I',bc,p); p+=4
    code=bc[p:p+code_len]; p+=code_len
    print("code_len=%d"%code_len)
    pc=0
    while pc<len(code):
        op=code[pc]; pc+=1
        nm=OPS[op] if op<len(OPS) else "?%d"%op
        if op in NOARG:
            print("  %04d %s"%(pc-1,nm))
        else:
            v,=struct.unpack_from('<i',code,pc); pc+=4
            print("  %04d %s %d"%(pc-5,nm,v))
    if p<len(bc):
        ns,=struct.unpack_from('<I',bc,p); p+=4
        print("strings=%d"%ns)
        for i in range(ns):
            sl,=struct.unpack_from('<I',bc,p); p+=4
            print("  str[%d]='%s'"%(i,bc[p:p+sl].decode('latin-1'))); p+=sl
if __name__=='__main__': main()
