/* =============================================================================
 *  minijs.c  -  see minijs.h for the contract and design rationale.
 *
 *  Implementation: a recursive-descent parser that evaluates expressions and
 *  statements directly (no AST), with a fixed symbol table of signed ints.
 *  Supported grammar (JS subset, integers only):
 *
 *      program    := statement*
 *      statement  := 'var' IDENT ['=' expr] ';'
 *                  | IDENT '=' expr ';'
 *                  | 'if' '(' expr ')' stmt ['else' stmt]
 *                  | 'while' '(' expr ')' stmt
 *                  | '{' statement* '}'
 *                  | 'return' expr
 *                  | expr ';'
 *      expr       := or
 *      or         := and ('||' and)*
 *      and        := eq  ('&&' eq )*
 *      eq         := rel (('=='|'!=') rel)*
 *      rel        := add (('<'|'>'|'<='|'>=') add)*
 *      add        := mul (('+'|'-') mul)*
 *      mul        := unary (('*'|'/'|'%') unary)*
 *      unary      := ('!'|'-') unary | primary
 *      primary    := NUMBER | IDENT | '(' expr ')' | 'return' expr
 *
 *  The "result" (last evaluated expression) is what the caller reads back.
 *  Every value is a signed int.  No floats, no objects, no closures -- this is
 *  an honest JS subset that fits a 32-bit-int / freestanding box.
 * ============================================================================= */
#ifndef _MINIJS_C
#define _MINIJS_C

#include "minijs.h"

/* ---- small pieces that differ between host tests and the -nostdlib PE ----- */
#ifdef MINIJS_HOST
#include <string.h>
#include <stddef.h>
#include <stdio.h>
#define MJS_MEMMOVE(d,s,n)  memmove((d),(s),(n))
#define MJS_STRLEN(s)       strlen((s))
#define MJS_STRCMP(a,b)     strcmp((a),(b))
#else
/* freestanding PE: ntbrowser.c provides these tiny helpers. */
extern void mjs_memmove(void*d,const void*s,int n);
extern int  mjs_strlen(const char*s);
#define MJS_MEMMOVE(d,s,n)   mjs_memmove((d),(s),(n))
#define MJS_STRLEN(s)        mjs_strlen((s))
#define MJS_STRCMP(a,b)      mjs_strncmp((a),(b))
extern int mjs_strncmp(const char*a,const char*b);
#endif

/* ---- tokenizer ------------------------------------------------------------- */
enum { TK_EOF=0, TK_NUM, TK_ID, TK_KW, TK_OP };
enum { OP_PLUS=1, OP_MINUS, OP_MUL, OP_DIV, OP_MOD,
       OP_LT, OP_GT, OP_LE, OP_GE, OP_EQ, OP_NE,
       OP_AND, OP_OR, OP_ASSIGN, OP_NOT, OP_LP, OP_RP,
       OP_LB, OP_RB, OP_SEMI, OP_COMMA };
enum { KW_IF=1, KW_ELSE, KW_WHILE, KW_VAR, KW_RETURN };

typedef struct {
    const char* p;
    int   tok;        /* TK_* */
    int   ival;       /* TK_NUM */
    char  sval[64];   /* TK_ID / TK_KW name */
    int   op;         /* TK_OP -> OP_* */
    int   kwn;        /* TK_KW -> KW_* */
    int   line;
    /* one-token pushback, used so mjs_statement can peek at the token after an
       identifier and decide assignment vs. plain expression without losing it */
    int   pb_tok, pb_ival, pb_op, pb_kwn, pb_line;
    char  pb_sval[64];
    const char* pb_p;
    int   has_pb;     /* 1 when the next token is the pushed-back one */
    const char* i_p;  /* where the current identifier started (for rewinds) */
} MJS_Lex;

static void lex_next(MJS_Lex* L);        /* fwd for lex_rewind below */
static void lex_rewind(MJS_Lex* L, const char* p); /* fwd */
static void lex_pop(MJS_Lex* L){
    if (L->has_pb){
        L->tok = L->pb_tok; L->ival = L->pb_ival; L->op = L->pb_op;
        L->kwn = L->pb_kwn; L->line = L->pb_line; L->p = L->pb_p;
        MJS_MEMMOVE(L->sval, L->pb_sval, (int)MJS_STRLEN(L->pb_sval) + 1);
        L->has_pb = 0;
    }
}
/* Back out to a saved source position and clear any pending pushback, so the
   parser can re-read from there (used to un-consume an identifier). */
static void lex_rewind(MJS_Lex* L, const char* p){
    L->has_pb = 0; L->p = p; lex_next(L);
}

static void lex_skip(MJS_Lex* L){
    while (*L->p==' '||*L->p=='\t'||*L->p=='\n'||*L->p=='\r'){ if(*L->p=='\n')L->line++; L->p++; }
    if (L->p[0]=='/' && L->p[1]=='/'){ while (*L->p && *L->p!='\n') L->p++; lex_skip(L); }
    else if (L->p[0]=='/' && L->p[1]=='*'){
        L->p+=2;
        while (*L->p && !(L->p[0]=='*'&&L->p[1]=='/')){ if(*L->p=='\n')L->line++; L->p++; }
        if (*L->p) L->p+=2;
        lex_skip(L);
    }
}

static void lex_next(MJS_Lex* L){
    if (L->has_pb){ lex_pop(L); return; }
    lex_skip(L);
    char c=*L->p;
    L->sval[0]=0;
    if (c==0){ L->tok=TK_EOF; return; }
    if (c>='0' && c<='9'){
        int v=0; while (*L->p>='0'&&*L->p<='9'){ v=v*10+(*L->p-'0'); L->p++; }
        L->tok=TK_NUM; L->ival=v; return;
    }
    if ((c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='_'){
        L->i_p = L->p;   /* remember where this identifier starts  */
        int i=0; while ((*L->p>='a'&&*L->p<='z')||(*L->p>='A'&&*L->p<='Z')||
                        (*L->p>='0'&&*L->p<='9')||*L->p=='_'){ if(i<63)L->sval[i++]=*L->p; L->p++; }
        L->sval[i]=0;
        if (MJS_STRCMP(L->sval,"if")==0)L->kwn=KW_IF;
        else if (MJS_STRCMP(L->sval,"else")==0)L->kwn=KW_ELSE;
        else if (MJS_STRCMP(L->sval,"while")==0)L->kwn=KW_WHILE;
        else if (MJS_STRCMP(L->sval,"var")==0)L->kwn=KW_VAR;
        else if (MJS_STRCMP(L->sval,"return")==0)L->kwn=KW_RETURN;
        else { L->tok=TK_ID; return; }
        L->tok=TK_KW; return;
    }
    L->p++;
    switch (c){
        case '+':L->tok=TK_OP;L->op=OP_PLUS; goto done;
        case '-':L->tok=TK_OP;L->op=OP_MINUS; goto done;
        case '*':L->tok=TK_OP;L->op=OP_MUL; goto done;
        case '/':L->tok=TK_OP;L->op=OP_DIV; goto done;
        case '%':L->tok=TK_OP;L->op=OP_MOD; goto done;
        case '(':L->tok=TK_OP;L->op=OP_LP; goto done2;
        case ')':L->tok=TK_OP;L->op=OP_RP; goto done2;
        case '{':L->tok=TK_OP;L->op=OP_LB; goto done2;
        case '}':L->tok=TK_OP;L->op=OP_RB; goto done2;
        case ';':L->tok=TK_OP;L->op=OP_SEMI; goto done2;
        case ',':L->tok=TK_OP;L->op=OP_COMMA; goto done2;
        case '!': if(*L->p=='='){L->p++;L->tok=TK_OP;L->op=OP_NE;} else {L->tok=TK_OP;L->op=OP_NOT;} goto done2;
        case '=': if(*L->p=='='){L->p++;L->tok=TK_OP;L->op=OP_EQ;} else {L->tok=TK_OP;L->op=OP_ASSIGN;} goto done2;
        case '<': if(*L->p=='='){L->p++;L->tok=TK_OP;L->op=OP_LE;} else {L->tok=TK_OP;L->op=OP_LT;} goto done2;
        case '>': if(*L->p=='='){L->p++;L->tok=TK_OP;L->op=OP_GE;} else {L->tok=TK_OP;L->op=OP_GT;} goto done2;
        case '&': L->p++; if(*L->p=='&'){L->p++;L->tok=TK_OP;L->op=OP_AND;} else L->tok=TK_EOF; goto done2;
        case '|': L->p++; if(*L->p=='|'){L->p++;L->tok=TK_OP;L->op=OP_OR;} else L->tok=TK_EOF; goto done2;
        default: L->tok=TK_EOF; goto done2;
    }
done:
done2:
    return;
}

/* ---- evaluator state -------------------------------------------------------- */
#define MJS_VARS 64
#define MJS_NAMELEN 20

typedef struct {
    char  names[MJS_VARS][MJS_NAMELEN];   /* fixed symbol table           */
    long  vals[MJS_VARS];
    int   n;
    MJS_Lex lex;
    int   result;             /* last evaluated expression value        */
    int   error;
    int   ret_flag;           /* a return halted the script early       */
    char* emsg;
    int   ecap;
} MJS;

static void mjs_err(MJS* S, const char* m){
    if (!S->error){ S->error=1; if (S->emsg && S->ecap>0){ int i=0; while(m[i]&&i<S->ecap-1){S->emsg[i]=m[i];i++;} S->emsg[i]=0; } }
}
static int mjs_var(MJS* S, const char* nm){
    int i;
    for (i=0;i<S->n;i++) if (MJS_STRCMP(S->names[i],nm)==0) return (int)S->vals[i];
    return 0;   /* undeclared reads as 0 (lenient) */
}
static void mjs_set(MJS* S, const char* nm, long v){
    int i;
    for (i=0;i<S->n;i++) if (MJS_STRCMP(S->names[i],nm)==0){ S->vals[i]=v; return; }
    if (S->n < MJS_VARS){ S->vals[S->n]=v; MJS_MEMMOVE(S->names[S->n],nm,(int)MJS_STRLEN(nm)+1); S->n++; }
}

typedef struct { MJS* S; long v; } E;

/* ---- expression parser (recursive descent, direct evaluation) -------------- */
static long mjs_unary(MJS* S);
static long mjs_expr_node(MJS* S);

static long mjs_primary(MJS* S){
    MJS_Lex* L=&S->lex;
    if (L->tok==TK_NUM){ long v=L->ival; lex_next(L); return v; }
    if (L->tok==TK_ID){ char nm[MJS_NAMELEN]; MJS_MEMMOVE(nm,L->sval,(int)MJS_STRLEN(L->sval)+1); lex_next(L); return mjs_var(S,nm); }
    if (L->tok==TK_KW && L->kwn==KW_RETURN){ lex_next(L); long v=mjs_expr_node(S); S->result=(int)v; S->ret_flag=1; return v; }
    if (L->tok==TK_OP && L->op==OP_LP){ lex_next(L); long v=mjs_expr_node(S); if (L->tok==TK_OP && L->op==OP_RP) lex_next(L); return v; }
    if (L->tok==TK_OP && L->op==OP_MINUS){ lex_next(L); return -mjs_unary(S); }
    if (L->tok==TK_OP && L->op==OP_NOT){ lex_next(L); return !mjs_unary(S); }
    mjs_err(S,"syntax"); return 0;
}
static long mjs_mul(MJS* S){
    long v=mjs_unary(S);
    for(;;){
        if(S->lex.tok==TK_OP && S->lex.op==OP_MUL){lex_next(&S->lex);v*=mjs_unary(S);}
        else if(S->lex.tok==TK_OP && S->lex.op==OP_DIV){lex_next(&S->lex);long r=mjs_unary(S);v=(r!=0)?v/r:0;}
        else if(S->lex.tok==TK_OP && S->lex.op==OP_MOD){lex_next(&S->lex);long r=mjs_unary(S);v=(r!=0)?v%r:0;}
        else return v;
    }
}
static long mjs_add(MJS* S){
    long v=mjs_mul(S);
    for(;;){
        if(S->lex.tok==TK_OP && S->lex.op==OP_PLUS){lex_next(&S->lex);v+=mjs_mul(S);}
        else if(S->lex.tok==TK_OP && S->lex.op==OP_MINUS){lex_next(&S->lex);v-=mjs_mul(S);}
        else return v;
    }
}
static long mjs_rel(MJS* S){
    long v=mjs_add(S);
    for(;;){
        if(S->lex.tok==TK_OP&&S->lex.op==OP_LT){lex_next(&S->lex);v=(v<mjs_add(S));}
        else if(S->lex.tok==TK_OP&&S->lex.op==OP_GT){lex_next(&S->lex);v=(v>mjs_add(S));}
        else if(S->lex.tok==TK_OP&&S->lex.op==OP_LE){lex_next(&S->lex);v=(v<=mjs_add(S));}
        else if(S->lex.tok==TK_OP&&S->lex.op==OP_GE){lex_next(&S->lex);v=(v>=mjs_add(S));}
        else return v;
    }
}
static long mjs_eq(MJS* S){
    long v=mjs_rel(S);
    for(;;){
        if(S->lex.tok==TK_OP&&S->lex.op==OP_EQ){lex_next(&S->lex);v=(v==mjs_rel(S));}
        else if(S->lex.tok==TK_OP&&S->lex.op==OP_NE){lex_next(&S->lex);v=(v!=mjs_rel(S));}
        else return v;
    }
}
static long mjs_and(MJS* S){
    long v=mjs_eq(S);
    while(S->lex.tok==TK_OP&&S->lex.op==OP_AND){lex_next(&S->lex);v=(v && mjs_eq(S));}
    return v;
}
static long mjs_or(MJS* S){
    long v=mjs_and(S);
    while(S->lex.tok==TK_OP&&S->lex.op==OP_OR){lex_next(&S->lex);v=(v || mjs_and(S));}
    return v;
}
static long mjs_unary(MJS* S){ return mjs_primary(S); }
static long mjs_expr_node(MJS* S){ return mjs_or(S); }

/* ---- statement parser ------------------------------------------------------- */
static void mjs_statement(MJS* S);
static void skip_block(MJS* S);

static void mjs_statement(MJS* S){
    MJS_Lex* L=&S->lex;
    if (L->tok==TK_KW && L->kwn==KW_VAR){
        lex_next(L);
        if (L->tok==TK_ID){
            char nm[MJS_NAMELEN]; MJS_MEMMOVE(nm,L->sval,(int)MJS_STRLEN(L->sval)+1); lex_next(L);
            if (L->tok==TK_OP && L->op==OP_ASSIGN){ lex_next(L); long v=mjs_expr_node(S); mjs_set(S,nm,v); S->result=(int)v; }
            else { mjs_set(S,nm,0); S->result=0; }
        }
        if (L->tok==TK_OP && L->op==OP_SEMI) lex_next(L);
        return;
    }
    if (L->tok==TK_ID){
        /* Identifier: if followed by '=' it's an assignment; otherwise we
           rewind to the ident and treat the whole thing as a plain expression
           so statements like `a % b;` or `x == 5;` evaluate fully. */
        const char* ident_p = L->i_p;    /* start of the identifier   */
        char nm[MJS_NAMELEN]; MJS_MEMMOVE(nm,L->sval,(int)MJS_STRLEN(L->sval)+1);
        lex_next(L);                      /* consume the ident       */
        if (L->tok==TK_OP && L->op==OP_ASSIGN){
            lex_next(L); long v=mjs_expr_node(S); mjs_set(S,nm,v); S->result=(int)v;
            if (L->tok==TK_OP && L->op==OP_SEMI) lex_next(L);
            return;
        }
        lex_rewind(L, ident_p);           /* re-read the ident expr  */
    }
    if (L->tok==TK_KW && L->kwn==KW_IF){
        lex_next(L);
        if (L->tok==TK_OP&&L->op==OP_LP) lex_next(L);
        long c=mjs_expr_node(S);
        if (L->tok==TK_OP&&L->op==OP_RP) lex_next(L);
        if (c) mjs_statement(S);
        else   skip_block(S);
        if (L->tok==TK_KW && L->kwn==KW_ELSE){
            lex_next(L); mjs_statement(S);
        }
        return;
    }
    if (L->tok==TK_KW && L->kwn==KW_WHILE){
        /* Bounded while: we run a real loop but cap iterations so a
           non-terminating script can't hang the kernel/browser.  On each pass
           we re-lex the condition + body from a saved source pointer. */
        const char* loop_p = L->p;      /* points at "while"               */
        int guard = 0;
        for (;;){
            if (++guard > 10000){ mjs_err(S,"loop limit"); break; }
            L->has_pb = 0; L->p = loop_p; lex_next(L);   /* "while"         */
            lex_next(L);                                  /* '('            */
            if (L->tok==TK_OP&&L->op==OP_RP) { /* empty () */ }
            long c=mjs_expr_node(S);
            if (L->tok==TK_OP&&L->op==OP_RP) lex_next(L);
            if (!c){
                /* Condition false: consume the (possibly brace-less) body so it
                   is not re-parsed as a fresh statement after the loop ends. */
                if (L->tok==TK_OP && L->op==OP_LB) skip_block(S);
                break;
            }
            /* run the body once */
            if (L->tok==TK_OP && L->op==OP_LB) lex_next(L);
            while (L->tok!=TK_EOF && !(L->tok==TK_OP&&L->op==OP_RB) && !S->error && !S->ret_flag)
                mjs_statement(S);
            if (L->tok==TK_OP && L->op==OP_RB) lex_next(L);
            if (S->error || S->ret_flag) break;
        }
        return;
    }
    if (L->tok==TK_OP && L->op==OP_LB){
        lex_next(L);
        while (L->tok!=TK_EOF && !(L->tok==TK_OP&&L->op==OP_RB) && !S->error && !S->ret_flag)
            mjs_statement(S);
        if (L->tok==TK_OP && L->op==OP_RB) lex_next(L);
        return;
    }
    /* default: evaluate an expression until ';' / '}' / EOF */
    long v=mjs_expr_node(S); S->result=(int)v;
    while (L->tok!=TK_EOF && !(L->tok==TK_OP&&L->op==OP_SEMI) && !(L->tok==TK_OP&&L->op==OP_RB)){ lex_next(L); }
    if (L->tok==TK_OP && L->op==OP_SEMI) lex_next(L);
}

static void skip_block(MJS* S){
    MJS_Lex* L=&S->lex;
    int depth=0;
    if (L->tok==TK_OP && L->op==OP_LB){ lex_next(L); depth=1; }
    while (!S->error && !S->ret_flag && L->tok!=TK_EOF){
        if (L->tok==TK_OP){
            if (L->op==OP_LB){ depth++; lex_next(L); continue; }
            if (L->op==OP_RB){ depth--; lex_next(L); if (depth<=0) break; continue; }
            if (L->op==OP_SEMI){ lex_next(L); continue; }
        }
        lex_next(L);
    }
}

int minijs_run(const char* src, int* out, char* errmsg, int cap){
    MJS st; int i;
    st.n=0; st.result=0; st.error=0; st.ret_flag=0; st.emsg=errmsg; st.ecap=cap;
    for (i=0;i<MJS_VARS;i++) st.names[i][0]=0;
    st.lex.p=src; st.lex.tok=0; st.lex.line=1; st.lex.has_pb=0;
    lex_next(&st.lex);
    while (st.lex.tok!=TK_EOF && !st.error && !st.ret_flag)
        mjs_statement(&st);
    if (out) *out = st.result;
    return st.error?1:0;
}

#endif /* _MINIJS_C */
