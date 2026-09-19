#!/usr/bin/env python3
# skillc.py - NexOS plugin source (.skill, C++ subset) -> bytecode (.bc, NBC1).
# Host-side tool (runs on the build machine).  Produces a binary blob that
# skill_vm.h can load and execute.  Supported subset:
#   types:   int, uint32_t, bool, char
#   control: if/else, while, for
#   funcs:   definition, calls, params, return
#   vars:    globals, locals, arrays (via mem), structs (flattened to globals)
#   ops:     + - * / %  == != < > <= >=  && || !  (bitwise later)
# Not supported: templates, virtual, exceptions, RTTI, pointers-to-functions.
import sys, struct

# ---------------- tokenizer ----------------
KEYWORDS = {'int','uint32_t','bool','char','if','else','while','for','return',
            'struct','void','true','false'}
OPS = ['==','!=','<=','>=','&&','||','++','--','->','<<','>>',
       '+','-','*','/','%','<','>','=','!','&','|','(',')','{','}','[',']',',',';','.']
class Tok:
    def __init__(s,k,v): s.k=k; s.v=v
def tokenize(src):
    i=0;n=len(src);t=[]
    while i<n:
        c=src[i]
        if c in ' \t\r\n': i+=1; continue
        if c=='/' and i+1<n and src[i+1]=='/':
            while i<n and src[i]!='\n': i+=1
            continue
        if c=='/' and i+1<n and src[i+1]=='*':
            i+=2
            while i+1<n and not(src[i]=='*' and src[i+1]=='/'): i+=1
            i+=2; continue
        if c.isalpha() or c=='_':
            j=i
            while j<n and (src[j].isalnum() or src[j]=='_'): j+=1
            w=src[i:j]; t.append(Tok('id' if w not in KEYWORDS else 'kw', w)); i=j; continue
        if c.isdigit():
            # Phase 4: support 0x hex literals (colours) as well as decimal.
            if c=='0' and i+1<n and src[i+1] in 'xX':
                j=i+2; h=''
                while j<n and src[j] in '0123456789abcdefABCDEF': h+=src[j]; j+=1
                t.append(Tok('num', int(h,16) if h else 0)); i=j; continue
            j=i
            while j<n and src[j].isdigit(): j+=1
            t.append(Tok('num', int(src[i:j]))); i=j; continue
        if c=='"':
            j=i+1; buf=''
            while j<n and src[j]!='"': buf+=src[j]; j+=1
            t.append(Tok('str', buf)); i=j+1; continue
        # operator
        matched=None
        for o in OPS:
            if src[i:i+len(o)]==o: matched=o; break
        if matched:
            t.append(Tok('op', matched)); i+=len(matched); continue
        raise SyntaxError("bad char %r at %d"%(c,i))
    t.append(Tok('eof',''))
    return t

# ---------------- compiler ----------------
class C:
    def __init__(self):
        self.code=bytearray()
        self.globals={}      # name->index
        self.ng=0
        self.funcs=[]        # list of dict(name,params,locals,entry,nparams)
        self.funcmap={}      # name->idx
        self.structs={}      # name->{fields:[(name,size)]}
        self.strings=[]      # Phase 4: literal string pool (in emit order)
        self.str_addr={}     # literal -> mem address (NBC_STR_BASE based)
        self.str_base=4096   # must match NBC_STR_BASE in skill_vm.h
        self.ginit=[]        # (global_index, tok_start, tok_end) initialisers
        self.memtop=0
        self.fixups=[]       # (pos, kind, target) for forward jumps/labels
    # emit
    def u32(self,v):
        self.code += struct.pack('<i', v & 0xFFFFFFFF)
    def emit(self,b): self.code.append(b)
    def emit_i32(self,v): self.code += struct.pack('<i', v & 0xFFFFFFFF)
    def here(self): return len(self.code)
    def patch(self,pos,v):
        self.code[pos:pos+4]=struct.pack('<i', v & 0xFFFFFFFF)
    # global
    def gnew(self,name):
        if name in self.globals: return self.globals[name]
        i=self.ng; self.ng+=1; self.globals[name]=i; return i
    def struct_field_off(self,name,field):
        # Phase 4: find which struct type owns `field` and return its index.
        # (We don't track variable->type, so we search every struct; field
        # names are assumed distinct across the program.)
        best=-1
        for ty,fields in self.structs.items():
            for idx,(fname,fsz) in enumerate(fields):
                if fname==field:
                    if best<0: best=idx
        if best<0:
            raise SyntaxError("struct field '%s' not found in any struct" % field)
        return best

class FuncCompiler:
    def __init__(self,c,toks,start):
        self.c=c; self.toks=toks; self.p=start; self.fidx=0
        self.locals={}; self.nl=0; self.params={}
    def peek(self): return self.toks[self.p]
    def nx(self): t=self.toks[self.p]; self.p+=1; return t
    def expect(self,k):
        t=self.nx()
        if k in "(){};[],=":
            if not (t.k=='op' and t.v==k):
                raise SyntaxError("expected %s got %s"%(k,t.v))
        else:
            if t.k!=k: raise SyntaxError("expected %s got %s"%(k,t.k))
    def is_op(self,s): return self.peek().k=='op' and self.peek().v==s
    def is_kw(self,s): return self.peek().k=='kw' and self.peek().v==s
    def local_new(self,name):
        if name in self.locals: return self.locals[name]
        i=self.nl; self.nl+=1; self.locals[name]=i; return i

    # ---- statements ----
    def compile(self, nparams):
        # params already registered before calling (by caller)
        self.compile_body()
        # implicit fall-off return: push 0 so RET always has a value to pop
        self.c.emit(OP_PUSH); self.c.emit_i32(0)
        self.c.emit(OP_RET); self.c.emit_i32(0)
    def compile_body(self):
        self.expect('{')
        while not self.is_op('}'):
            self.stmt()
        self.nx()  # }
    def stmt(self):
        t=self.peek()
        if t.k=='kw' and t.v in ('int','uint32_t','bool','char'):
            self.local_decl()
        elif t.k=='kw' and t.v=='if':
            self.if_stmt()
        elif t.k=='kw' and t.v=='while':
            self.while_stmt()
        elif t.k=='kw' and t.v=='for':
            self.for_stmt()
        elif t.k=='kw' and t.v=='return':
            self.nx(); 
            if self.is_op(';'):
                self.c.emit(OP_PUSH); self.c.emit_i32(0)   # void return -> push 0
                self.c.emit(OP_RET); self.c.emit_i32(0)
            else:
                self.expr()
                self.c.emit(OP_RET); self.c.emit_i32(0)  # value already on stack
            self.expect(';')
        elif t.k=='id' and self.toks[self.p+1].k=='op' and self.toks[self.p+1].v=='(':
            self.expr(); self.c.emit(OP_POP); self.expect(';')   # call expr as statement
        elif t.k=='id':
            self.assign_or_call()
        else:
            # expression statement
            self.expr(); self.expect(';')
    def local_decl(self):
        self.nx()  # type
        while True:
            name=self.nx().v
            idx=self.local_new(name)
            if self.is_op('='):
                self.nx(); self.expr(); self.c.emit(OP_STORE_L); self.c.emit_i32(idx)
            else:
                # ensure slot zeroed
                self.c.emit(OP_PUSH); self.c.emit_i32(0); self.c.emit(OP_STORE_L); self.c.emit_i32(idx)
            if self.is_op(','): self.nx(); continue
            break
        self.expect(';')
    def if_stmt(self):
        self.nx()
        self.expect('('); self.expr(); self.expect(')')
        jz=self.c.here(); self.c.emit(OP_JZ); self.c.emit_i32(0)
        self.stmt()
        if self.is_kw('else'):
            self.nx()
            jmp=self.c.here(); self.c.emit(OP_JMP); self.c.emit_i32(0)
            # patch the OPERAND (opcode+1); VM adds r to pc after reading it
            self.c.patch(jz+1, self.c.here()-(jz+1)-4)
            self.stmt()
            self.c.patch(jmp+1, self.c.here()-(jmp+1)-4)
        else:
            self.c.patch(jz+1, self.c.here()-(jz+1)-4)
    def while_stmt(self):
        self.nx(); start=self.c.here()
        self.expect('('); self.expr(); self.expect(')')
        jz=self.c.here(); self.c.emit(OP_JZ); self.c.emit_i32(0)
        self.stmt()
        jmp=self.c.here(); self.c.emit(OP_JMP); self.c.emit_i32(start-(jmp+1)-4)
        self.c.patch(jz+1, self.c.here()-(jz+1)-4)
    def for_stmt(self):
        self.nx(); self.expect('(')
        # init
        if not self.is_op(';'):
            if self.peek().k=='kw':
                self.local_decl()
            else:
                self.assign_or_call()
        else:
            self.nx()
        # cond
        cond=self.c.here()
        if not self.is_op(';'):
            self.expr()
        else:
            self.c.emit(OP_PUSH); self.c.emit_i32(1)
        self.expect(';')
        jz=self.c.here(); self.c.emit(OP_JZ); self.c.emit_i32(0)
        # step (parse but emit after body)
        step_start=self.c.here()
        if not self.is_op(')'):
            self.expr()
        else:
            self.c.emit(OP_PUSH); self.c.emit_i32(0)
        self.expect(')')
        step_code=self.c.code[self.step_start:]  # save step bytes
        del self.c.code[self.step_start:]
        # body
        self.stmt()
        # emit step then jump to cond
        self.c.code += step_code
        jmp=self.c.here(); self.c.emit(OP_JMP); self.c.emit_i32(cond-(jmp+1)-4)
        self.c.patch(jz+1, self.c.here()-(jz+1)-4)

    def assign_or_call(self):
        name=self.nx().v
        if self.is_op('('):   # call as statement
            nargs=self.call_args()
            self.c.emit(OP_CALL); self.c.emit_i32(self.call_idx(name))
            self.c.emit(OP_POP)   # discard return value
            return
        # assignment: name ('=' | '[' idx ']=' | '.field=' )
        # array / member
        base_local = name in self.locals
        if self.is_op('['):
            self.nx()
            if base_local:
                self.c.emit(OP_LOAD_L); self.c.emit_i32(self.locals[name])
            else:
                self.c.emit(OP_LOAD_G); self.c.emit_i32(self.c.globals[name])
            self.expr(); self.expect(']')
            self.expect('='); self.expr()
            self.c.emit(OP_STORE_IDX)
            self.expect(';'); return
        if self.is_op('.'):
            self.nx(); field=self.nx().v
            # struct member -> global base+field
            base=self.c.globals[name]
            off=self.c.struct_field_off(name, field)
            self.expect('='); self.expr()
            self.c.emit(OP_STORE_G); self.c.emit_i32(base+off)
            self.expect(';'); return
        self.expect('='); self.expr()
        if base_local:
            self.c.emit(OP_STORE_L); self.c.emit_i32(self.locals[name])
        else:
            self.c.emit(OP_STORE_G); self.c.emit_i32(self.c.globals[name])
        self.expect(';')

    def call_idx(self,name):
        # Phase 4: an undefined callee is a hard compile error, not a 0xFFFF
        # sentinel that would fault only at run time.
        if name in self.c.funcmap: return self.c.funcmap[name]
        raise SyntaxError("call to undefined function '%s'" % name)
    def call_args(self):
        self.expect('('); n=0
        if not self.is_op(')'):
            while True:
                self.expr(); n+=1
                if self.is_op(','): self.nx(); continue
                break
        self.expect(')'); return n

    # ---- expressions ----
    def expr(self): self.assignment()
    def assignment(self):
        # handled above for simple ids; here handle general: just parse binary
        self.bor()
    def bor(self):
        self.land()
        while self.is_op('||'):
            self.nx(); self.land(); self.c.emit(OP_OR)
    def land(self):
        self.band()
        while self.is_op('&&'):
            self.nx(); self.band(); self.c.emit(OP_AND)
    def band(self):
        self.eq()
        while self.is_op('&'):
            self.nx(); self.eq(); self.c.emit(OP_AND)
    def eq(self):
        self.cmp()
        while self.is_op('==') or self.is_op('!='):
            o=self.nx().v; self.cmp()
            self.c.emit(OP_EQ if o=='==' else OP_NE)
    def cmp(self):
        self.bshift()
        while self.is_op('<') or self.is_op('>') or self.is_op('<=') or self.is_op('>='):
            o=self.nx().v; self.bshift()
            self.c.emit({'<':OP_LT,'>':OP_GT,'<=':OP_LE,'>=':OP_GE}[o])
    def bshift(self):
        self.add()
        while self.is_op('<<') or self.is_op('>>'):
            o=self.nx().v; self.add()
            self.c.emit(OP_SHL if o=='<<' else OP_SHR)
    def add(self):
        self.mul()
        while self.is_op('+') or self.is_op('-'):
            o=self.nx().v; self.mul()
            self.c.emit(OP_ADD if o=='+' else OP_SUB)
    def mul(self):
        self.unary()
        while self.is_op('*') or self.is_op('/') or self.is_op('%'):
            o=self.nx().v; self.unary()
            self.c.emit({'*':OP_MUL,'/':OP_DIV,'%':OP_MOD}[o])
    def unary(self):
        if self.is_op('-'):
            self.nx(); self.unary(); self.c.emit(OP_NEG)
        elif self.is_op('!'):
            self.nx(); self.unary(); self.c.emit(OP_NOT)
        elif (self.peek().k=='op' and self.peek().v=='(' and
              self.toks[self.p+1].k=='kw' and
              self.toks[self.p+1].v in ('int','uint32_t','bool','char')):
            # type cast: (type) expr -> identity in our int VM
            self.nx(); self.nx(); self.expect(')'); self.unary()
        else:
            self.primary()
    def primary(self):
        t=self.peek()
        if t.k=='num':
            self.nx(); self.c.emit(OP_PUSH); self.c.emit_i32(t.v)
        elif t.k=='str':
            # Phase 4: literal strings are pooled into the data section and the
            # address (into NBC_MEM at NBC_STR_BASE) is pushed.  The VM copies
            # the bytes into mem[] at load time.
            self.nx()
            s = t.v
            if s in self.c.str_addr:
                addr = self.c.str_addr[s]
            else:
                addr = self.c.str_base
                for o in self.c.strings: addr += len(o) + 1
                self.c.str_addr[s] = addr
                self.c.strings.append(s)
            self.c.emit(OP_PUSH); self.c.emit_i32(addr)
        elif self.is_op('('):
            self.nx(); self.expr(); self.expect(')')
        elif t.k=='id':
            self.nx()
            if self.is_op('('):   # call
                nargs=self.call_args()
                self.c.emit(OP_CALL); self.c.emit_i32(self.call_idx(t.v))
                # CALL leaves the return value on the stack (used as expression result)
            elif self.is_op('['):  # array index read
                self.nx()
                if t.v in self.locals:
                    self.c.emit(OP_LOAD_L); self.c.emit_i32(self.locals[t.v])
                else:
                    self.c.emit(OP_LOAD_G); self.c.emit_i32(self.c.globals[t.v])
                self.expr(); self.expect(']')
                self.c.emit(OP_LOAD_IDX)
            elif self.is_op('.'):  # struct member read
                self.nx(); field=self.nx().v
                base=self.c.globals[t.v]; off=self.c.struct_field_off(t.v, field)
                self.c.emit(OP_LOAD_G); self.c.emit_i32(base+off)
            else:
                if t.v in self.locals:
                    self.c.emit(OP_LOAD_L); self.c.emit_i32(self.locals[t.v])
                elif t.v in self.c.globals:
                    self.c.emit(OP_LOAD_G); self.c.emit_i32(self.c.globals[t.v])
                else:
                    raise SyntaxError("undefined var %s at tok %d"%(t.v, self.p))
        else:
            raise SyntaxError("unexpected token %s"%t.k)

# I referenced dummy push; remove that mistake: after CALL, return value is on stack already.
# (We'll strip the erroneous push by re-emitting correctly in a cleaner version.)

# ---- top level driver ----
OP_NOP=0;OP_PUSH=1;OP_POP=2;OP_ADD=3;OP_SUB=4;OP_MUL=5;OP_DIV=6;OP_MOD=7;OP_NEG=8
OP_EQ=9;OP_NE=10;OP_LT=11;OP_GT=12;OP_LE=13;OP_GE=14;OP_AND=15;OP_OR=16;OP_NOT=17
OP_LOAD_L=18;OP_STORE_L=19;OP_LOAD_G=20;OP_STORE_G=21
OP_JMP=22;OP_JZ=23;OP_JNZ=24;OP_CALL=25;OP_RET=26;OP_HALT=27
OP_DUP=28;OP_LOAD_MEM=29;OP_STORE_MEM=30;OP_LOAD_IDX=31;OP_STORE_IDX=32
OP_SHL=33;OP_SHR=34

def compile_src(src):
    toks=tokenize(src)
    c=C()
    # pass 1: collect structs, globals, function signatures
    i=0
    # helper to find top-level by scanning tokens with brace matching
    pos=0
    def parse_toplevel():
        nonlocal pos
        # skip type/struct
        t=toks[pos]
        if t.k=='kw' and t.v=='struct':
            pos+=1; name=toks[pos].v; pos+=1
            # { fields }
            assert toks[pos].v=='{'; pos+=1
            fields=[]
            # handle "int x, y; int z;" -> a type followed by one or more names
            while toks[pos].v!='}':
                if toks[pos].v==';':         # stray separator between decls
                    pos+=1; continue
                ftype=toks[pos].v; pos+=1   # e.g. int
                while True:
                    fname=toks[pos].v; pos+=1
                    fields.append((fname,1))
                    if toks[pos].v==',': pos+=1; continue
                    break
                if toks[pos].v==';': pos+=1
            pos+=1  # }
            c.structs[name]=fields
            return
        # global var or function
        typ=toks[pos].v; pos+=1
        name=toks[pos].v; pos+=1
        if toks[pos].v=='(':
            # function
            pos+=1; params=[]
            if toks[pos].v!=')':
                while True:
                    pt=toks[pos].v; pos+=1; pname=toks[pos].v; pos+=1
                    params.append(pname)
                    if toks[pos].v==',': pos+=1; continue
                    break
            pos+=1  # )
            idx=len(c.funcs)
            c.funcs.append({'name':name,'params':params,'locals':0,'entry':0,'nparams':len(params)})
            c.funcmap[name]=idx
            # skip body
            depth=0
            while True:
                tk=toks[pos]
                if tk.v=='{': depth+=1
                elif tk.v=='}':
                    depth-=1
                    if depth==0: pos+=1; break
                pos+=1
        else:
            # global var (maybe array / with init)
            gi = c.gnew(name)
            if toks[pos].v=='[':
                while toks[pos].v!=']': pos+=1
                pos+=1
            # record initializer expression tokens (Phase 4: globals were
            # previously left at 0 because the initialiser was skipped)
            if toks[pos].v=='=':
                pos+=1
                start=pos
                while toks[pos].v!=';': pos+=1
                c.ginit.append((gi, start, pos))
                pos+=1
                return
            # skip to ;
            while toks[pos].v!=';': pos+=1
            pos+=1
    while toks[pos].k!='eof':
        parse_toplevel()
    # pass 2: compile each function
    # We need to re-tokenize bodies. Simpler: re-run parse but compile.
    # Reuse the same token stream; restart pos and compile function bodies by
    # locating them.
    pos=0
    def find_func_body(name):
        nonlocal pos
        while toks[pos].k!='eof':
            if toks[pos].k=='kw' and toks[pos].v in ('int','uint32_t','bool','char','void'):
                save=pos; pos+=1
                if toks[pos].v==name and toks[pos+1].v=='(':
                    # skip to '{'
                    while toks[pos].v!='{': pos+=1
                    return save
                pos=save
            pos+=1
        return -1
    for idx,f in enumerate(c.funcs):
        # locate start
        p=find_func_body(f['name'])
        # compile using FuncCompiler starting at the type token
        fc=FuncCompiler(c,toks,p)
        # register params
        for pn in f['params']:
            fc.params[pn]=fc.local_new(pn)
        # advance fc.p to '{'
        while fc.peek().v!='{': fc.p+=1
        entry=c.here()
        c.funcs[idx]['entry']=entry
        # Phase 4: materialise global initialisers at the top of main() so that
        # running main (or a reload hook invoking main) sets the globals.
        if f['name']=='main':
            for (gi, ts, te) in c.ginit:
                sub=FuncCompiler(c,toks,ts)
                sub.expr()
                c.emit(OP_STORE_G); c.emit_i32(gi)
        # We must compile the body. Use fc.compile but it expects to start at '{'.
        try:
            fc.compile(f['nparams'])
        except Exception as e:
            raise SyntaxError("func %s: %s (near %s)"%(f['name'], e, fc.peek().v))
        c.funcs[idx]['locals']=fc.nl
    # build output
    out=bytearray()
    out += b'NBC1'
    out += struct.pack('<I',1)             # version
    out += struct.pack('<I',c.ng)          # nglobals
    out += struct.pack('<I',len(c.funcs))  # nfuncs
    for f in c.funcs:
        nm=f['name'].encode('ascii')[:19]
        nb=bytearray(20); nb[:len(nm)]=nm; out+=nb
        out+=struct.pack('<iii', f['entry'], f['nparams'], f['locals'])
    # Phase 4: explicit code length, then code, then a string table.
    out += struct.pack('<I', len(c.code))   # code length
    out += c.code
    out += struct.pack('<I', len(c.strings)) # string count
    for s in c.strings:
        sb = s.encode('latin-1')
        out += struct.pack('<I', len(sb))
        out += sb
    return bytes(out)

if __name__=='__main__':
    if len(sys.argv)<3:
        print("usage: skillc.py <in.skill> <out.bc>"); sys.exit(1)
    src=open(sys.argv[1]).read()
    bc=compile_src(src)
    open(sys.argv[2],'wb').write(bc)
    print("skillc: %d bytes -> %s"%(len(bc),sys.argv[2]))
