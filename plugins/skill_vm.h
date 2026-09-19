/* skill_vm.h - NexOS bytecode interpreter (freestanding, no stdlib).
 *
 * Stack machine "NBC1".  Kept header-only so the SAME code runs inside the
 * kernel (plugin) and on the host (test harness) -- that lets us verify the
 * VM executes real .bc without booting QEMU.
 *
 * Bytecode layout:
 *   magic "NBC1" | u32 version | u32 nglobals | u32 nfuncs
 *   per func: u8 name[20] (nul-term) | u32 entry | u32 nparams | u32 nlocals
 *   code:    stream of u8 opcodes + i32/operands (little-endian)
 *
 * Call convention: CALL f expects its nparams already pushed.  A frame pushes
 * (old bp, return-pc); locals live above the params.  RET returns one int.
 */
#ifndef SKILL_VM_H
#define SKILL_VM_H
#include <stdint.h>

#define NBC_MAGIC  0x3143424E  /* "NBC1" little-endian */
#define NBC_VERSION 1
#define NBC_STACK  4096
#define NBC_MEM    65536        /* linear 'memory' for arrays/pointers */
#define NBC_FRAMES 64
#define NBC_LOCALS 64
#define NBC_STR_BASE 4096      /* string literals live here in 'mem' */

/* Opcodes */
enum {
    OP_NOP=0, OP_PUSH, OP_POP, OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD, OP_NEG,
    OP_EQ, OP_NE, OP_LT, OP_GT, OP_LE, OP_GE, OP_AND, OP_OR, OP_NOT,
    OP_LOAD_L, OP_STORE_L, OP_LOAD_G, OP_STORE_G,
    OP_JMP, OP_JZ, OP_JNZ, OP_CALL, OP_RET, OP_HALT,
    OP_DUP, OP_LOAD_MEM, OP_STORE_MEM, OP_LOAD_IDX, OP_STORE_IDX,
    OP_SHL, OP_SHR
};

typedef struct {
    int globals[NBC_MEM];          /* global variables */
    int mem[NBC_MEM];              /* array / pointer storage */
    int stk[NBC_STACK];
    int sp;
    const uint8_t* code;
    int code_len;
    /* func table (filled by loader) */
    int f_entry[NBC_FRAMES];
    int f_params[NBC_FRAMES];
    int f_locals[NBC_FRAMES];
    char f_name[NBC_FRAMES][20];
    int nf;
} SkillVM;

/* Look a function up by name; returns its index or -1. */
static int skill_find(SkillVM* vm, const char* name) {
    for (int i = 0; i < vm->nf; i++) {
        const char* a = vm->f_name[i];
        const char* b = name;
        while (*a && *a == *b) { a++; b++; }
        if (*a == 0 && *b == 0) return i;
    }
    return -1;
}

/* Load a .bc buffer; returns 0 on success, <0 on bad magic/version. */
static int skill_load(SkillVM* vm, const uint8_t* bc, int len) {
    if (len < 16) return -1;
    uint32_t magic = ((uint32_t)bc[0]) | ((uint32_t)bc[1]<<8) | ((uint32_t)bc[2]<<16) | ((uint32_t)bc[3]<<24);
    if (magic != NBC_MAGIC) return -2;
    uint32_t ver = ((uint32_t)bc[4]) | ((uint32_t)bc[5]<<8) | ((uint32_t)bc[6]<<16) | ((uint32_t)bc[7]<<24);
    if (ver != NBC_VERSION) return -3;
    uint32_t ng = ((uint32_t)bc[8]) | ((uint32_t)bc[9]<<8) | ((uint32_t)bc[10]<<16) | ((uint32_t)bc[11]<<24);
    uint32_t nf = ((uint32_t)bc[12]) | ((uint32_t)bc[13]<<8) | ((uint32_t)bc[14]<<16) | ((uint32_t)bc[15]<<24);
    (void)ng;
    if (nf > NBC_FRAMES) return -4;
    int p = 16;
    for (uint32_t i = 0; i < nf; i++) {
        for (int k = 0; k < 20; k++) vm->f_name[i][k] = (char)bc[p + k];
        vm->f_name[i][19] = 0;
        p += 20;                                   /* skip name */
        vm->f_entry[i]  = (int)(bc[p] | (bc[p+1]<<8) | (bc[p+2]<<16) | (bc[p+3]<<24)); p+=4;
        vm->f_params[i] = (int)(bc[p] | (bc[p+1]<<8) | (bc[p+2]<<16) | (bc[p+3]<<24)); p+=4;
        vm->f_locals[i] = (int)(bc[p] | (bc[p+1]<<8) | (bc[p+2]<<16) | (bc[p+3]<<24)); p+=4;
    }
    vm->nf = (int)nf;
    /* explicit code length, then code, then a string table (Phase 4) */
    uint32_t code_len = ((uint32_t)bc[p]) | ((uint32_t)bc[p+1]<<8) | ((uint32_t)bc[p+2]<<16) | ((uint32_t)bc[p+3]<<24); p += 4;
    vm->code = bc + p;
    vm->code_len = (int)code_len;
    p += (int)code_len;
    /* reset VM state BEFORE loading strings, so the string pool survives */
    for (int i = 0; i < NBC_STACK; i++) vm->stk[i] = 0;
    for (int i = 0; i < NBC_MEM; i++) { vm->globals[i] = 0; vm->mem[i] = 0; }
    vm->sp = 0;
    /* string table: u32 count, then per string u32 len + bytes (null-terminated) */
    if (p + 4 <= len) {
        uint32_t ns = ((uint32_t)bc[p]) | ((uint32_t)bc[p+1]<<8) | ((uint32_t)bc[p+2]<<16) | ((uint32_t)bc[p+3]<<24); p += 4;
        uint32_t addr = NBC_STR_BASE;
        for (uint32_t i = 0; i < ns; i++) {
            uint32_t sl = ((uint32_t)bc[p]) | ((uint32_t)bc[p+1]<<8) | ((uint32_t)bc[p+2]<<16) | ((uint32_t)bc[p+3]<<24); p += 4;
            for (uint32_t j = 0; j < sl && addr < NBC_MEM; j++) vm->mem[addr++] = bc[p++];
            if (addr < NBC_MEM) vm->mem[addr++] = 0;  /* null terminator */
        }
    }
    return 0;
}

/* Read a little-endian i32 from code at *pc, advance *pc. */
static int skill_rd_i32(const uint8_t* c, int* pc) {
    int v = (int)(c[*pc] | (c[*pc+1]<<8) | (c[*pc+2]<<16) | (c[*pc+3]<<24));
    *pc += 4; return v;
}

/* Run function 'fid' with nargs already pushed as params. Returns int result
 * (0 on normal completion via HALT).  out_val receives HALT's operand. */
static int skill_run(SkillVM* vm, int fid, int* out_val) {
    int pc = vm->f_entry[fid];
    int bp = 0;          /* base pointer: params start at stk[bp] */
    int frames = 0;
    int ret_val = 0;
    int halt_val = 0;
    /* we use an explicit call stack of (fid, retpc, retbp) */
    int call_fid[NBC_FRAMES], call_pc[NBC_FRAMES], call_bp[NBC_FRAMES];

    for (;;) {
        uint8_t op = vm->code[pc++];
        switch (op) {
        case OP_PUSH: { int v = skill_rd_i32(vm->code, &pc); vm->stk[vm->sp++] = v; break; }
        case OP_POP:  vm->sp--; break;
        case OP_DUP:  vm->stk[vm->sp] = vm->stk[vm->sp-1]; vm->sp++; break;
        case OP_ADD: { int b=vm->stk[--vm->sp], a=vm->stk[--vm->sp]; vm->stk[vm->sp++]=a+b; } break;
        case OP_SUB: { int b=vm->stk[--vm->sp], a=vm->stk[--vm->sp]; vm->stk[vm->sp++]=a-b; } break;
        case OP_MUL: { int b=vm->stk[--vm->sp], a=vm->stk[--vm->sp]; vm->stk[vm->sp++]=a*b; } break;
        case OP_DIV: { int b=vm->stk[--vm->sp], a=vm->stk[--vm->sp]; vm->stk[vm->sp++]=(b==0)?0:a/b; } break;
        case OP_MOD: { int b=vm->stk[--vm->sp], a=vm->stk[--vm->sp]; vm->stk[vm->sp++]=(b==0)?0:a%b; } break;
        case OP_NEG: { int a=vm->stk[--vm->sp]; vm->stk[vm->sp++]=-a; } break;
        case OP_EQ: { int b=vm->stk[--vm->sp], a=vm->stk[--vm->sp]; vm->stk[vm->sp++]=(a==b); } break;
        case OP_NE: { int b=vm->stk[--vm->sp], a=vm->stk[--vm->sp]; vm->stk[vm->sp++]=(a!=b); } break;
        case OP_LT: { int b=vm->stk[--vm->sp], a=vm->stk[--vm->sp]; vm->stk[vm->sp++]=(a<b); } break;
        case OP_GT: { int b=vm->stk[--vm->sp], a=vm->stk[--vm->sp]; vm->stk[vm->sp++]=(a>b); } break;
        case OP_LE: { int b=vm->stk[--vm->sp], a=vm->stk[--vm->sp]; vm->stk[vm->sp++]=(a<=b); } break;
        case OP_GE: { int b=vm->stk[--vm->sp], a=vm->stk[--vm->sp]; vm->stk[vm->sp++]=(a>=b); } break;
        case OP_AND:{ int b=vm->stk[--vm->sp], a=vm->stk[--vm->sp]; vm->stk[vm->sp++]=(a&&b); } break;
        case OP_OR: { int b=vm->stk[--vm->sp], a=vm->stk[--vm->sp]; vm->stk[vm->sp++]=(a||b); } break;
        case OP_NOT:{ int a=vm->stk[--vm->sp]; vm->stk[vm->sp++]=!a; } break;
        case OP_LOAD_L: { int i = (uint16_t)skill_rd_i32(vm->code, &pc); vm->stk[vm->sp++] = vm->stk[bp + i]; } break;
        case OP_STORE_L:{ int i = (uint16_t)skill_rd_i32(vm->code, &pc); int v = vm->stk[--vm->sp]; vm->stk[bp + i] = v; } break;
        case OP_LOAD_G: { int i = (uint16_t)skill_rd_i32(vm->code, &pc); vm->stk[vm->sp++] = vm->globals[i]; } break;
        case OP_STORE_G:{ int i = (uint16_t)skill_rd_i32(vm->code, &pc); int v = vm->stk[--vm->sp]; vm->globals[i] = v; } break;
        case OP_LOAD_MEM:{ int a = skill_rd_i32(vm->code, &pc); vm->stk[vm->sp++] = vm->mem[a]; } break;
        case OP_STORE_MEM:{ int a = skill_rd_i32(vm->code, &pc); int v = vm->stk[--vm->sp]; vm->mem[a] = v; } break;
        case OP_LOAD_IDX:{ int i = vm->stk[--vm->sp]; int b = vm->stk[--vm->sp]; vm->stk[vm->sp++] = vm->mem[b + i]; } break;
        case OP_STORE_IDX:{ int v = vm->stk[--vm->sp]; int i = vm->stk[--vm->sp]; int b = vm->stk[--vm->sp]; vm->mem[b + i] = v; } break;
        case OP_JMP: { int r = skill_rd_i32(vm->code, &pc); pc += r; } break;
        case OP_JZ:  { int r = skill_rd_i32(vm->code, &pc); int c = vm->stk[--vm->sp]; if (!c) pc += r; } break;
        case OP_JNZ: { int r = skill_rd_i32(vm->code, &pc); int c = vm->stk[--vm->sp]; if (c) pc += r; } break;
        case OP_CALL: {
            int f = (uint16_t)skill_rd_i32(vm->code, &pc);
            if (frames >= NBC_FRAMES) return -10;
            call_fid[frames] = fid; call_pc[frames] = pc; call_bp[frames] = bp; frames++;
            fid = f; pc = vm->f_entry[fid]; bp = vm->sp - vm->f_params[fid];
            /* allocate locals */
            for (int k = 0; k < vm->f_locals[fid]; k++) vm->stk[vm->sp++] = 0;
            break;
        }
        case OP_RET: {
            /* The compiler pushes the return value on the stack, so RET pops
             * it.  (A 4-byte immediate follows RET in the code stream as
             * padding; it is never executed.) */
            int v = (vm->sp > 0) ? vm->stk[--vm->sp] : 0;
            ret_val = v;
            if (frames == 0) { halt_val = v; if (out_val) *out_val = v; return 0; }   /* top-level return */
            frames--;
            vm->sp = bp;                 /* drop params+locals */
            fid = call_fid[frames]; pc = call_pc[frames]; bp = call_bp[frames];
            vm->stk[vm->sp++] = v;       /* return value */
            break;
        }
        case OP_HALT:{ halt_val = skill_rd_i32(vm->code, &pc); if (out_val) *out_val = halt_val; return 0; }
        case OP_NOP: default: break;
        }
    }
}

#endif /* SKILL_VM_H */
