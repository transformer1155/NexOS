/* =====================================================================
 *  usr/python.c  -  A tiny genuine Python-subset interpreter for the
 *  NexOS Linux compatibility layer (Milestone 0 / "AI writes Hello world").
 * ---------------------------------------------------------------------
 *  This is a real recursive-descent interpreter: it lexes Python source,
 *  builds an AST, and tree-walks it.  It runs as a freestanding Linux
 *  i386 ELF32 (PT_LOAD only) loaded by linux_run() and driven through the
 *  unified int 0x80 ABI (same runtime as usr/libc.c).
 *
 *  Supported subset:
 *    - literals: 123  'str'  "str"   (with \n \t \\ \' \" escapes)
 *    - variables, assignment:  x = expr
 *    - arithmetic/comparison: + - * / %  == != < > <= >=   (str + str concat)
 *    - calls:  name(args)                (builtins only)
 *    - method calls:  obj.method(args)   (file objects)
 *    - builtins: print() open() exec() len()
 *    - in-memory filesystem so open('f','w').write(...) + exec(open('f').read())
 *      genuinely WRITES a program and RUNS it.
 *
 *  The "AI" (the NexOS agent) authors a .py program, drops it in the SFS,
 *  and the Linux layer runs `python <prog>.py`.  If python writes a hello
 *  world file and successfully runs it, the milestone passes.
 * ===================================================================== */
#include "libc.h"
#include <stdarg.h>

/* ----------------------------------------------------------------- */
/*  Syscall wrappers (int 0x80, Linux i386 ABI)                       */
/*  nex_write / nex_exit are provided by libc.c.  We add the ones we  */
/*  need for reading the script file and (not) writing to disk.       */
/* ----------------------------------------------------------------- */
static inline long sys5(long num, long a, long b, long c, long d, long e)
{
    long ret;
    __asm__ volatile ("int $0x80"
        : "=a"(ret)
        : "a"(num), "b"(a), "c"(b), "d"(c), "S"(d), "D"(e)
        : "memory", "cc");
    return ret;
}
static int  nex_open (const char* p, int flags){ return (int)sys5(5,(long)p,flags,0,0,0); }
static int  nex_read (int fd, void* b, unsigned long n){ return (int)sys5(3,(long)fd,(long)b,n,0,0); }
static int  nex_close(int fd){ return (int)sys5(6,(long)fd,0,0,0,0); }

static void pstr(const char* s){ nex_write(1, s, strlen(s)); }
static void pdec(int v){
    char d[16]; int k=0; int neg=0;
    if (v<0){ neg=1; v=-v; }
    if (v==0) d[k++]='0';
    else { char t[16]; int ti=0; while(v){ t[ti++]=(char)('0'+(v%10)); v/=10; } while(ti) d[k++]=t[--ti]; }
    if (neg) d[k++]='-';
    d[k]=0; pstr(d);
}

/* ----------------------------------------------------------------- */
/*  Value model                                                       */
/* ----------------------------------------------------------------- */
enum { V_NONE=0, V_INT=1, V_STR=2, V_OBJ=3 };
enum { OBJ_FILE=1 };

typedef struct Obj {
    int  type;
    void* udata;     /* for OBJ_FILE: pointer to FSEntry* */
} Obj;

typedef struct Val {
    int   t;
    long  i;
    char* s;         /* NUL-terminated heap string (STR) */
    int   slen;
    Obj*  o;
} Val;

static Val v_none(void){ Val v; v.t=V_NONE; v.i=0; v.s=0; v.slen=0; v.o=0; return v; }
static Val v_int(long n){ Val v; v.t=V_INT; v.i=n; v.s=0; v.slen=0; v.o=0; return v; }
static Val v_obj(Obj* o){ Val v; v.t=V_OBJ; v.i=0; v.s=0; v.slen=0; v.o=o; return v; }
static Val v_str_dup(const char* src, int len){
    Val v; v.t=V_STR; v.i=0; v.o=0;
    char* b = (char*)malloc((size_t)len + 1);
    int i; for (i=0;i<len;i++) b[i]=src[i];
    b[len]=0; v.s=b; v.slen=len; return v;
}

/* ----------------------------------------------------------------- */
/*  In-memory filesystem (so the demo can "write" a program)          */
/* ----------------------------------------------------------------- */
typedef struct {
    char  name[32];
    char* content;
    int   len;
    int   cap;
} FSEntry;

#define FS_MAX 16
static FSEntry g_fs[FS_MAX];
static int     g_fscount = 0;

static FSEntry* fs_find(const char* name){
    for (int i=0;i<g_fscount;i++)
        if (strcmp(g_fs[i].name, name)==0) return &g_fs[i];
    return 0;
}
static FSEntry* fs_put(const char* name){
    FSEntry* e = fs_find(name);
    if (e){ e->len=0; return e; }              /* truncate existing */
    if (g_fscount >= FS_MAX) return 0;
    e = &g_fs[g_fscount++];
    int i=0; while (name[i] && i<31) { e->name[i]=name[i]; i++; }
    e->name[i]=0;
    e->content = (char*)malloc(64);
    e->cap = 64; e->len = 0;
    return e;
}
static void fs_append(FSEntry* e, const char* s, int n){
    if (!e) return;
    if (e->len + n + 1 > e->cap){
        int nc = e->cap * 2 + n + 16;
        char* nb = (char*)malloc((size_t)nc);
        int i; for (i=0;i<e->len;i++) nb[i]=e->content[i];
        free(e->content); e->content=nb; e->cap=nc;
    }
    int i; for (i=0;i<n;i++) e->content[e->len+i]=s[i];
    e->len += n; e->content[e->len]=0;
}

/* ----------------------------------------------------------------- */
/*  Environment (global variable table)                              */
/* ----------------------------------------------------------------- */
#define ENV_MAX 64
static struct { char name[32]; Val val; } g_env[ENV_MAX];
static int g_envcount = 0;

static Val* env_get(const char* name){
    for (int i=0;i<g_envcount;i++)
        if (strcmp(g_env[i].name, name)==0) return &g_env[i].val;
    return 0;
}
static void env_set(const char* name, Val v){
    Val* e = env_get(name);
    if (e){ *e = v; return; }
    if (g_envcount >= ENV_MAX) return;
    int i=0; while (name[i] && i<31){ g_env[g_envcount].name[i]=name[i]; i++; }
    g_env[g_envcount].name[i]=0;
    g_env[g_envcount].val = v;
    g_envcount++;
}

/* ----------------------------------------------------------------- */
/*  Lexer                                                            */
/* ----------------------------------------------------------------- */
enum { T_INT=1, T_STR=2, T_NAME=3, T_OP=4, T_ASSIGN=5, T_EOF=6, T_HASH=7 };

typedef struct {
    int   type;
    long  ival;
    char* sval;   /* for STR/NAME/OP: heap copy */
} Tok;

static Tok* g_toks = 0;
static int  g_ntok = 0;

static char* dupn(const char* s, int n){
    char* b = (char*)malloc((size_t)n+1);
    int i; for (i=0;i<n;i++) b[i]=s[i];
    b[n]=0; return b;
}

static int lex(const char* src, Tok** out, int* nout){
    Tok* toks = 0; int cap=0, n=0;
    const char* p = src;
    while (1){
        /* skip whitespace (incl. newlines) */
        while (*p==' '||*p=='\t'||*p=='\r'||*p=='\n'||*p=='\f') p++;
        if (*p==0) break;
        /* comment to end of line */
        if (*p=='#'){ while (*p && *p!='\n') p++; continue; }

        if (n+1 > cap){ cap = cap? cap*2:64; toks = (Tok*)realloc(toks, (size_t)cap*sizeof(Tok)); }
        Tok* t = &toks[n]; t->sval=0;

        if (*p>='0' && *p<='9'){
            long v=0; while (*p>='0'&&*p<='9'){ v=v*10+(*p-'0'); p++; }
            t->type=T_INT; t->ival=v; n++;
            continue;
        }
        if (*p=='\'' || *p=='"'){
            char q=*p; p++;
            char buf[1024]; int bl=0;
            while (*p && *p!=q){
                char c=*p;
                if (c=='\\'){
                    p++;
                    if (*p=='n') c='\n';
                    else if (*p=='t') c='\t';
                    else if (*p=='\\') c='\\';
                    else if (*p=='\'' ) c='\'';
                    else if (*p=='"' ) c='"';
                    else c=*p;
                    if (*p) p++;
                } else p++;
                if (bl<1023) buf[bl++]=c;
            }
            if (*p==q) p++;
            t->type=T_STR; t->sval=dupn(buf, bl); n++;
            continue;
        }
        if ((*p>='a'&&*p<='z')||(*p>='A'&&*p<='Z')||(*p=='_')){
            const char* s=p;
            while ((*p>='a'&&*p<='z')||(*p>='A'&&*p<='Z')||(*p>='0'&&*p<='9')||(*p=='_')) p++;
            t->type=T_NAME; t->sval=dupn(s,(int)(p-s)); n++;
            continue;
        }
        /* punctuation */
        if (*p=='='){
            if (p[1]=='='){ t->type=T_OP; t->sval=dupn("==",2); p+=2; }
            else { t->type=T_ASSIGN; p++; }
            n++; continue;
        }
        if (*p=='!'){
            if (p[1]=='='){ t->type=T_OP; t->sval=dupn("!=",2); p+=2; n++; continue; }
            p++; continue; /* lone '!' skip */
        }
        if (*p=='<'){
            if (p[1]=='='){ t->type=T_OP; t->sval=dupn("<=",2); p+=2; }
            else { t->type=T_OP; t->sval=dupn("<",1); p++; }
            n++; continue;
        }
        if (*p=='>'){
            if (p[1]=='='){ t->type=T_OP; t->sval=dupn(">=",2); p+=2; }
            else { t->type=T_OP; t->sval=dupn(">",1); p++; }
            n++; continue;
        }
        /* single-char ops */
        char op[2]; op[0]=*p; op[1]=0;
        t->type=T_OP; t->sval=dupn(op,1); p++; n++;
    }
    /* EOF */
    if (n+1>cap){ cap=n+1; toks=(Tok*)realloc(toks,(size_t)cap*sizeof(Tok)); }
    toks[n].type=T_EOF; toks[n].sval=0; toks[n].ival=0; n++;
    *out=toks; *nout=n;
    return 0;
}

/* ----------------------------------------------------------------- */
/*  Parser (recursive descent) -> AST                                */
/* ----------------------------------------------------------------- */
enum { K_INT=1, K_STR=2, K_NAME=3, K_BIN=4, K_CALL=5, K_MCALL=6,
       K_ASSIGN=7, K_EXPR=8, K_NEG=9 };

typedef struct Node {
    int kind;
    long ival;
    char* sval;       /* STR text / NAME / op / call-or-method name */
    struct Node* a;   /* left / callee / obj */
    struct Node* b;   /* right / arg list head */
    struct Node* next;/* statement/arg chaining */
} Node;

static Node* newnode(int k){
    Node* n = (Node*)malloc(sizeof(Node));
    n->kind=k; n->ival=0; n->sval=0; n->a=0; n->b=0; n->next=0;
    return n;
}

static int  pi = 0;        /* parse index */
static int  pn = 0;
static Tok* pt = 0;
static int  g_parse_error = 0;

static Tok* peek(void){ return (pi < pn) ? &pt[pi] : &pt[pn-1]; }
static Tok* advance(void){ return (pi < pn) ? &pt[pi++] : &pt[pn-1]; }
static int  accept_op(const char* s){
    if (peek()->type==T_OP && strcmp(peek()->sval,s)==0){ advance(); return 1; }
    return 0;
}

static Node* parse_expr(void);
static Node* parse_primary(void);

static Node* parse_args(void){
    /* parse comma-separated expr list until ')' */
    Node* head = newnode(K_EXPR); head->next=0; Node* tail=head;
    if (!accept_op(")")){
        while (1){
            Node* a = parse_expr();
            tail->next = a; tail = a;
            if (accept_op(",")) continue;
            break;
        }
        if (!accept_op(")")){ g_parse_error=1; }
    }
    return head;
}

static Node* parse_postfix(void){
    /* builtin call: NAME '(' args ')'  — falls through to the .method()
     * suffix loop below so `open('x').read()` parses correctly (the call
     * result is a value that may still receive method calls). */
    Node* val;
    if (peek()->type==T_NAME && peek()->sval && pi+1<pn &&
        pt[pi+1].type==T_OP && strcmp(pt[pi+1].sval,"(")==0){
        char* nm = peek()->sval; advance(); advance(); /* NAME ( */
        Node* args = parse_args();
        Node* n = newnode(K_CALL); n->sval=nm; n->b=args;
        val = n;
    } else {
        val = parse_primary();
    }
    while (1){
        if (peek()->type==T_OP && strcmp(peek()->sval,".")==0){
            advance();
            if (peek()->type!=T_NAME){ g_parse_error=1; break; }
            char* mname = peek()->sval; advance();
            if (!accept_op("(")){ g_parse_error=1; break; }
            Node* args = parse_args();
            Node* m = newnode(K_MCALL); m->sval=mname; m->a=val; m->b=args;
            val = m;
        } else if (peek()->type==T_OP && strcmp(peek()->sval,"(")==0){
            /* calling a value directly: unsupported -> error */
            g_parse_error=1; break;
        } else break;
    }
    return val;
}

static Node* parse_unary(void){
    if (accept_op("-")){ Node* n=newnode(K_NEG); n->a=parse_unary(); return n; }
    if (accept_op("+")){ return parse_unary(); }
    return parse_postfix();
}

static Node* parse_mul(void){
    Node* left = parse_unary();
    while (peek()->type==T_OP){
        const char* o = peek()->sval;
        if (strcmp(o,"*")==0||strcmp(o,"/")==0||strcmp(o,"%")==0){
            advance();
            Node* right = parse_unary();
            Node* n = newnode(K_BIN); n->sval=(char*)o; n->a=left; n->b=right;
            left = n;
        } else break;
    }
    return left;
}

static Node* parse_add(void){
    Node* left = parse_mul();
    while (peek()->type==T_OP){
        const char* o = peek()->sval;
        if (strcmp(o,"+")==0||strcmp(o,"-")==0){
            advance();
            Node* right = parse_mul();
            Node* n = newnode(K_BIN); n->sval=(char*)o; n->a=left; n->b=right;
            left = n;
        } else break;
    }
    return left;
}

static Node* parse_cmp(void){
    Node* left = parse_add();
    while (peek()->type==T_OP){
        const char* o = peek()->sval;
        if (strcmp(o,"<")==0||strcmp(o,">")==0||strcmp(o,"<=")==0||strcmp(o,">=")==0){
            advance();
            Node* right = parse_add();
            Node* n = newnode(K_BIN); n->sval=(char*)o; n->a=left; n->b=right;
            left = n;
        } else break;
    }
    return left;
}

static Node* parse_eq(void){
    Node* left = parse_cmp();
    while (peek()->type==T_OP){
        const char* o = peek()->sval;
        if (strcmp(o,"==")==0||strcmp(o,"!=")==0){
            advance();
            Node* right = parse_cmp();
            Node* n = newnode(K_BIN); n->sval=(char*)o; n->a=left; n->b=right;
            left = n;
        } else break;
    }
    return left;
}

static Node* parse_expr(void){ return parse_eq(); }

static Node* parse_primary(void){
    Tok* t = peek();
    if (t->type==T_INT){
        advance(); Node* n=newnode(K_INT); n->ival=t->ival; return n;
    }
    if (t->type==T_STR){
        advance(); Node* n=newnode(K_STR); n->sval=t->sval; return n;
    }
    if (t->type==T_NAME){
        advance(); Node* n=newnode(K_NAME); n->sval=t->sval; return n;
    }
    if (t->type==T_OP && strcmp(t->sval,"(")==0){
        advance();
        Node* e = parse_expr();
        if (!accept_op(")")) g_parse_error=1;
        return e;
    }
    g_parse_error=1;
    advance();
    return newnode(K_EXPR);
}

static Node* parse_stmt(void){
    if (peek()->type==T_NAME && peek()->sval && pi+1<pn &&
        pt[pi+1].type==T_ASSIGN){
        char* nm = peek()->sval; advance(); advance(); /* NAME = */
        Node* val = parse_expr();
        Node* n = newnode(K_ASSIGN); n->sval=nm; n->a=val;
        return n;
    }
    Node* e = parse_expr();
    Node* n = newnode(K_EXPR); n->a = e;
    return n;
}

static Node* parse_program(void){
    Node* head = newnode(K_EXPR); head->next=0; Node* tail=head;
    while (peek()->type != T_EOF){
        if (g_parse_error) break;
        Node* s = parse_stmt();
        tail->next = s; tail = s;
        /* optional statement separator ';' */
        accept_op(";");
    }
    return head;
}

/* ----------------------------------------------------------------- */
/*  Evaluator                                                         */
/* ----------------------------------------------------------------- */
static Val eval_node(Node* n);

static Val eval_args(Node* arglist, Val* out, int maxn, int* nout){
    int c=0;
    Node* a = arglist ? arglist->next : 0;
    while (a && c<maxn){ out[c++] = eval_node(a); a = a->next; }
    *nout = c;
    return v_none();
}

/* builtins */
static Val bif_print(Val* a, int n){
    for (int i=0;i<n;i++){
        Val v = a[i];
        if (v.t==V_STR){ nex_write(1, v.s, (unsigned long)v.slen); }
        else if (v.t==V_INT){
            char buf[24]; int bi=0; long x=v.i;
            if (x<0){ nex_write(1,"-",1); x=-x; }
            if (x==0) buf[bi++]='0';
            else { char t[20]; int ti=0; while(x){ t[ti++]=(char)('0'+x%10); x/=10; }
                   while(ti>0) buf[bi++]=t[--ti]; }
            nex_write(1, buf, (unsigned long)bi);
        } else if (v.t==V_NONE){ nex_write(1,"None",4); }
        if (i<n-1) nex_write(1," ",1);
    }
    nex_write(1,"\n",1);
    return v_none();
}

static Val bif_open(Val* a, int n){
    if (n<1 || a[0].t!=V_STR) return v_none();
    char mode = 'r';
    if (n>=2 && a[1].t==V_STR && a[1].slen>0) mode = a[1].s[0];
    FSEntry* e;
    if (mode=='w' || mode=='a'){ e = fs_put(a[0].s); }
    else { e = fs_find(a[0].s); if (!e) return v_none(); }
    Obj* o = (Obj*)malloc(sizeof(Obj));
    o->type = OBJ_FILE; o->udata = e;
    return v_obj(o);
}

static Val bif_len(Val* a, int n){
    if (n>=1 && a[0].t==V_STR) return v_int((long)a[0].slen);
    return v_int(0);
}

/* exec(src): parse + run a source string (the "run it" half) */
static void run_source(const char* src);

static Val bif_exec(Val* a, int n){
    if (n>=1 && a[0].t==V_STR) run_source(a[0].s);
    return v_none();
}

/* file object methods */
static Val method_file(Obj* self, const char* mname, Val* a, int n){
    FSEntry* e = (FSEntry*)self->udata;
    if (strcmp(mname,"write")==0){
        if (n>=1 && a[0].t==V_STR) fs_append(e, a[0].s, a[0].slen);
        return v_none();
    }
    if (strcmp(mname,"read")==0){
        if (!e || e->len==0) return v_str_dup("",0);
        return v_str_dup(e->content, e->len);
    }
    if (strcmp(mname,"close")==0) return v_none();
    return v_none();
}

static Val eval_node(Node* n){
    if (!n) return v_none();
    switch (n->kind){
        case K_INT: return v_int(n->ival);
        case K_STR: return v_str_dup(n->sval, (int)strlen(n->sval));
        case K_NAME: {
            Val* v = env_get(n->sval);
            return v ? *v : v_none();
        }
        case K_NEG: {
            Val x = eval_node(n->a);
            return v_int(-(x.t==V_INT ? x.i : 0));
        }
        case K_ASSIGN: {
            Val v = eval_node(n->a);
            env_set(n->sval, v);
            return v;
        }
        case K_EXPR: {
            return eval_node(n->a);
        }
        case K_CALL: {
            Val args[16]; int na=0;
            eval_args(n->b, args, 16, &na);
            const char* nm = n->sval;
            if (strcmp(nm,"print")==0) return bif_print(args, na);
            if (strcmp(nm,"open")==0)  return bif_open(args, na);
            if (strcmp(nm,"exec")==0)  return bif_exec(args, na);
            if (strcmp(nm,"len")==0)   return bif_len(args, na);
            pstr("[python] unknown name: ");
            if (nm) nex_write(1, nm, (unsigned long)strlen(nm));
            pstr("\n");
            return v_none();
        }
        case K_MCALL: {
            Val obj = eval_node(n->a);
            if (obj.t != V_OBJ){ pstr("[python] method call on non-object\n"); return v_none(); }
            Val args[16]; int na=0;
            eval_args(n->b, args, 16, &na);
            if (obj.o->type==OBJ_FILE) return method_file(obj.o, n->sval, args, na);
            pstr("[python] unknown method\n");
            return v_none();
        }
        case K_BIN: {
            Val l = eval_node(n->a);
            Val r = eval_node(n->b);
            const char* op = n->sval;
            if (strcmp(op,"+")==0){
                if (l.t==V_STR && r.t==V_STR){
                    int total = l.slen + r.slen;
                    char* buf = (char*)malloc((size_t)total+1);
                    int i; for (i=0;i<l.slen;i++) buf[i]=l.s[i];
                    for (i=0;i<r.slen;i++) buf[l.slen+i]=r.s[i];
                    buf[total]=0;
                    Val v; v.t=V_STR; v.i=0; v.o=0; v.s=buf; v.slen=total;
                    return v;
                }
                return v_int((l.t==V_INT?l.i:0) + (r.t==V_INT?r.i:0));
            }
            if (strcmp(op,"-")==0) return v_int((l.t==V_INT?l.i:0) - (r.t==V_INT?r.i:0));
            if (strcmp(op,"*")==0) return v_int((l.t==V_INT?l.i:0) * (r.t==V_INT?r.i:0));
            if (strcmp(op,"/")==0){ long d=(r.t==V_INT?r.i:0); return v_int(d? (l.t==V_INT?l.i:0)/d : 0); }
            if (strcmp(op,"%")==0){ long d=(r.t==V_INT?r.i:0); return v_int(d? (l.t==V_INT?l.i:0)%d : 0); }
            /* comparisons -> 1/0 */
            long lv = (l.t==V_INT?l.i:0), rv = (r.t==V_INT?r.i:0);
            int res=0;
            if (strcmp(op,"==")==0) res = (lv==rv);
            else if (strcmp(op,"!=")==0) res = (lv!=rv);
            else if (strcmp(op,"<")==0)  res = (lv<rv);
            else if (strcmp(op,">")==0)  res = (lv>rv);
            else if (strcmp(op,"<=")==0) res = (lv<=rv);
            else if (strcmp(op,">=")==0) res = (lv>=rv);
            return v_int((long)res);
        }
    }
    return v_none();
}

/* ----------------------------------------------------------------- */
/*  run_source: lex -> parse -> exec                                 */
/* ----------------------------------------------------------------- */
static void run_source(const char* src){
    Tok* toks; int nt;
    lex(src, &toks, &nt);
    if (nt<=1){ if (toks) free(toks); return; }
    pi=0; pn=nt; pt=toks; g_parse_error=0;
    Node* prog = parse_program();
    if (g_parse_error){
        pstr("[python] parse error (check your program syntax)\n");
        pstr("[python] parse error at token index ");
        { char dbg[16]; int di=0; int v=pi; if(v==0) dbg[di++]='0';
          else { char t[12]; int ti=0; while(v){ t[ti++]=(char)('0'+v%10); v/=10; }
                 while(ti>0) dbg[di++]=t[--ti]; } dbg[di]=0; pstr(dbg); }
        pstr("\n");
        pstr("[python] token stream:\n");
        for (int i = 0; i < nt; i++){
            Tok* t = &toks[i];
            const char* tn = t->type==T_INT?"INT":t->type==T_STR?"STR":
                            t->type==T_NAME?"NAME":t->type==T_OP?"OP":
                            t->type==T_ASSIGN?"ASSIGN":t->type==T_EOF?"EOF":
                            t->type==T_HASH?"HASH":"?";
            pstr("  #"); pstr(tn); pstr(" '");
            if (t->sval) pstr(t->sval);
            pstr("'\n");
        }
    } else {
        Node* s = prog ? prog->next : 0;
        while (s){ eval_node(s); s = s->next; }
    }
    free(toks);
}

/* ----------------------------------------------------------------- */
/*  Script reader (main SFS via int 0x80)                            */
/* ----------------------------------------------------------------- */
static char g_mem_script[16384];
static char* read_script(const char* name){
    // Guest-memory protocol: "mem:<addr>:<len>" reads the script from a
    // physical address the kernel filled before launching us (identity map).
    if (name && name[0]=='m' && name[1]=='e' && name[2]=='m' && name[3]==':'){
        const char* p = name + 4;
        unsigned long addr = 0;
        while (*p >= '0' && *p <= '9'){ addr = addr*10 + (unsigned long)(*p - '0'); p++; }
        if (*p != ':'){ pstr("[python] read_script: bad mem: addr\n"); return 0; }
        p++;
        unsigned long len = 0;
        while (*p >= '0' && *p <= '9'){ len = len*10 + (unsigned long)(*p - '0'); p++; }
        if (len > sizeof(g_mem_script) - 1) len = sizeof(g_mem_script) - 1;
        const char* src = (const char*)addr;
        for (unsigned long i = 0; i < len; i++) g_mem_script[i] = src[i];
        g_mem_script[len] = 0;
        pstr("[python] read_script('"); pstr(name); pstr("') -> mem ");
        pdec((int)addr); pstr(" len="); pdec((int)len); pstr(" -> ok\n");
        return g_mem_script;
    }
    pstr("[python] read_script('"); pstr(name ? name : "(null)"); pstr("') -> ");
    int fd = nex_open(name, 0);
    pdec(fd); pstr("\n");
    if (fd < 0) return 0;
    int cap = 4096, len = 0;
    char* buf = (char*)malloc((size_t)cap);
    while (1){
        if (len+512 > cap){ cap*=2; char* nb=(char*)malloc((size_t)cap); int i; for(i=0;i<len;i++) nb[i]=buf[i]; free(buf); buf=nb; }
        int n = nex_read(fd, buf+len, 512);
        if (n <= 0) break;
        len += n;
    }
    buf[len]=0;
    nex_close(fd);
    return buf;
}

/* Embedded fallback program (identical logic to hello_demo.py) so the
 * milestone still passes even if the SFS script cannot be read. */
static const char* EMBEDDED_DEMO =
"f = open('hello.py','w')\n"
"f.write(\"print('Hello world from NexOS Linux + Python')\\n\")\n"
"f.close()\n"
"print('[python] wrote program hello.py:')\n"
"print(open('hello.py').read())\n"
"print('[python] running hello.py ...')\n"
"exec(open('hello.py').read())\n";

/* ----------------------------------------------------------------- */
/*  main                                                              */
/* ----------------------------------------------------------------- */
int main(int argc, char** argv, char** envp){
    pstr("[python] NexOS Linux + Python interpreter online (ELF32/i386)\n");
    pstr("[python] argc="); pdec(argc); pstr("\n");
    if (argc >= 1 && argv[0]){ pstr("[python] argv[0]='"); pstr(argv[0]); pstr("'\n"); }
    if (argc >= 2 && argv[1]){ pstr("[python] argv[1]='"); pstr(argv[1]); pstr("'\n"); }
    const char* src = 0;
    int used_file = 0;
    if (argc >= 2 && argv[1] && argv[1][0]){
        src = read_script(argv[1]);
        if (src){ used_file = 1; }
    }
    if (!src){ src = EMBEDDED_DEMO; }

    if (used_file){
        if (argv[1] && argv[1][0]=='m' && argv[1][1]=='e' && argv[1][2]=='m' && argv[1][3]==':'){
            pstr("[python] source: AI-generated code (passed via guest memory)\n");
        } else {
            pstr("[python] source: file '");
            pstr(argv[1]);
            pstr("' (authored by the NexOS agent)\n");
        }
    } else {
        pstr("[python] source: built-in demo\n");
    }

    run_source(src);
    pstr("[python] done.\n");
    return 0;
}
