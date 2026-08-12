// =====================================================================
//  clr.cpp  -  MiniCLR: a CIL (.NET IL) interpreter for NexOS
// ---------------------------------------------------------------------
//  Managed apps are authored in real C#, compiled by real Roslyn, and
//  flattened by tools/mex_pack.py into a .mex image.  The packer has
//  already resolved every ECMA-335 metadata token into a direct index:
//
//      ldstr  <string index>      call   <method index>
//      ldfld  <byte offset>       ldsfld <static slot>
//      newarr <type index>
//
//  so this file needs no metadata engine at all -- just a loader for a
//  few flat arrays plus an evaluation-stack interpreter.
//
//  Object layout (all references are raw 32-bit pointers, null == 0):
//      normal   [0]=type index  [4..]=fields
//      string   [0]=0xFFFFFFFE  [4]=byte length  [8..]=UTF-8 + NUL
//      array    [0]=0xFFFFFFFD  [4]=count  [8]=elem size  [12..]=data
// =====================================================================
#include "clr.h"

extern "C" void* kmalloc(uint32_t size);

// ---------------------------------------------------------------------
//  freestanding helpers (this translation unit has no libc)
// ---------------------------------------------------------------------
static inline void outb_(uint16_t p, uint8_t v) {
    __asm__ volatile("outb %0, %1" :: "a"(v), "Nd"(p));
}
static void ser(const char* s) { while (*s) outb_(0x3F8, (uint8_t)*s++); }
static void ser_ch(char c)     { outb_(0x3F8, (uint8_t)c); }

static void ser_int(int32_t v) {
    char b[16]; int i = 0;
    if (v < 0) { ser_ch('-'); v = -v; }
    if (v == 0) { ser_ch('0'); return; }
    while (v > 0 && i < 15) { b[i++] = (char)('0' + (v % 10)); v /= 10; }
    while (i > 0) ser_ch(b[--i]);
}
static void ser_hex(uint32_t v) {
    const char* h = "0123456789ABCDEF";
    ser("0x");
    for (int i = 28; i >= 0; i -= 4) ser_ch(h[(v >> i) & 0xF]);
}

static int  streq_(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}
static int  slen_(const char* s) { int n = 0; while (s[n]) n++; return n; }
static void scpy_(char* d, const char* s, int cap) {
    int i = 0; while (s[i] && i < cap - 1) { d[i] = s[i]; i++; } d[i] = 0;
}
static void scat_(char* d, const char* s, int cap) {
    int i = slen_(d); int j = 0;
    while (s[j] && i < cap - 1) d[i++] = s[j++];
    d[i] = 0;
}
static void mcpy_(void* d, const void* s, int n) {
    uint8_t* a = (uint8_t*)d; const uint8_t* b = (const uint8_t*)s;
    for (int i = 0; i < n; i++) a[i] = b[i];
}
static void mset_(void* d, int v, int n) {
    uint8_t* a = (uint8_t*)d; for (int i = 0; i < n; i++) a[i] = (uint8_t)v;
}

// ---------------------------------------------------------------------
//  .mex image format (must match tools/mex_pack.py)
// ---------------------------------------------------------------------
struct MexHeader {
    char     magic[4];       // "MEX1"
    uint32_t version;
    uint32_t entry;
    uint32_t n_methods;
    uint32_t off_methods;
    uint32_t n_types;
    uint32_t off_types;
    uint32_t n_statics;
    uint32_t n_strings;
    uint32_t off_strings;
    uint32_t off_litdata;
    uint32_t litdata_size;
    uint32_t off_code;
    uint32_t code_size;
    uint32_t n_icalls;
    uint32_t off_icalls;
    uint32_t off_names;
    uint32_t pad;
};

struct MexMethod {
    uint32_t code_off;
    uint32_t code_size;
    uint16_t max_stack;
    uint16_t n_locals;
    uint16_t n_args;
    uint16_t flags;          // 1=static 2=icall 4=ctor 8=has return value
    uint32_t icall_id;
    uint32_t decl_type;
    uint32_t name_off;       // "Namespace.Type::Method"
    uint32_t short_off;      // "Method" -- the key for virtual dispatch
};

struct MexType {
    uint32_t inst_size;
    uint32_t name_off;
    uint32_t base_type;
    uint32_t flags;
};

struct MexStr { uint32_t off, len; };

#define MF_STATIC   1
#define MF_ICALL    2
#define MF_CTOR     4
#define MF_HASRET   8
#define MF_ABSTRACT 16   // interface/abstract slot: no IL, no native handler
#define MF_VIRTUAL  32   // participates in callvirt dispatch

#define OBJ_STRING 0xFFFFFFFEu
#define OBJ_ARRAY  0xFFFFFFFDu

// ---------------------------------------------------------------------
//  runtime state
// ---------------------------------------------------------------------
#define CLR_IMAGE_MAX   (192 * 1024)
#define CLR_HEAP_SIZE   (512 * 1024)
#define CLR_STACK_SLOTS 4096
#define CLR_MAX_DEPTH   48
#define CLR_MAX_STRINGS 512
#define CLR_MAX_STATICS 512
#define CLR_STEP_LIMIT  20000000

static clr_read_fn  g_reader = 0;
static uint8_t*     g_image  = 0;
static MexHeader*   g_hdr    = 0;
static MexMethod*   g_methods = 0;
static MexType*     g_types  = 0;
static MexStr*      g_strtab = 0;
static uint8_t*     g_code   = 0;
static const char*  g_names  = 0;
static uint8_t*     g_litdata = 0;

static uint8_t*     g_heap = 0;
static uint32_t     g_heap_used = 0;

static int32_t      g_stack[CLR_STACK_SLOTS];
static int          g_sp = 0;

static int32_t      g_statics[CLR_MAX_STATICS];
static int32_t      g_strobj[CLR_MAX_STRINGS];
static int16_t      g_icall_bind[128];

static int          g_fault = 0;
static char         g_report[512];
static uint32_t     g_steps = 0;

static const char* mex_name(uint32_t off) { return g_names + off; }

static void fault(const char* msg) {
    if (g_fault) return;
    g_fault = 1;
    scpy_(g_report, "CLR fault: ", sizeof(g_report));
    scat_(g_report, msg, sizeof(g_report));
    ser("[CLR] fault: "); ser(msg); ser("\n");
}

// ---------------------------------------------------------------------
//  managed heap - bump allocator (no GC; apps are short lived)
// ---------------------------------------------------------------------
static uint8_t* heap_alloc(uint32_t size) {
    size = (size + 3) & ~3u;
    if (g_heap_used + size > CLR_HEAP_SIZE) {
        fault("managed heap exhausted");
        return 0;
    }
    uint8_t* p = g_heap + g_heap_used;
    g_heap_used += size;
    mset_(p, 0, (int)size);
    return p;
}

static int32_t make_string(const char* utf8, int len) {
    uint8_t* o = heap_alloc(8 + (uint32_t)len + 1);
    if (!o) return 0;
    *(uint32_t*)o = OBJ_STRING;
    *(uint32_t*)(o + 4) = (uint32_t)len;
    mcpy_(o + 8, utf8, len);
    o[8 + len] = 0;
    return (int32_t)(uint32_t)o;
}

static const char* str_data(int32_t s) { return (const char*)(uint32_t)s + 8; }
static int32_t     str_len(int32_t s)  { return (int32_t)*(uint32_t*)((uint32_t)s + 4); }

// =====================================================================
//  internal calls -- the managed <-> native boundary
//  Names must match the [MethodImpl(InternalCall)] declarations in
//  csharp/NexOS.Core/Sys.cs
// =====================================================================
typedef int32_t (*IcallFn)(int32_t* a);

static int32_t ic_print(int32_t* a) {
    if (a[0]) ser(str_data(a[0]));
    return 0;
}
static int32_t ic_print_int(int32_t* a) { ser_int(a[0]); return 0; }
static int32_t ic_print_char(int32_t* a) { ser_ch((char)a[0]); return 0; }

static int32_t ic_str_len(int32_t* a) { return a[0] ? str_len(a[0]) : 0; }

static int32_t ic_str_char_at(int32_t* a) {
    if (!a[0]) return 0;
    int32_t n = str_len(a[0]);
    if (a[1] < 0 || a[1] >= n) return 0;
    return (int32_t)(uint8_t)str_data(a[0])[a[1]];
}

static int32_t ic_str_concat(int32_t* a) {
    int la = a[0] ? str_len(a[0]) : 0;
    int lb = a[1] ? str_len(a[1]) : 0;
    uint8_t* o = heap_alloc(8 + (uint32_t)(la + lb) + 1);
    if (!o) return 0;
    *(uint32_t*)o = OBJ_STRING;
    *(uint32_t*)(o + 4) = (uint32_t)(la + lb);
    if (la) mcpy_(o + 8, str_data(a[0]), la);
    if (lb) mcpy_(o + 8 + la, str_data(a[1]), lb);
    o[8 + la + lb] = 0;
    return (int32_t)(uint32_t)o;
}

static int32_t ic_str_eq(int32_t* a) {
    // null-safe UTF-8 byte compare; both null => equal, one null => not.
    if (a[0] == a[1]) return 1;            // same ref (covers both null)
    if (!a[0] || !a[1]) return 0;
    int la = str_len(a[0]);
    int lb = str_len(a[1]);
    if (la != lb) return 0;
    const char* pa = str_data(a[0]);
    const char* pb = str_data(a[1]);
    for (int i = 0; i < la; i++) if (pa[i] != pb[i]) return 0;
    return 1;
}

static int32_t ic_int_to_str(int32_t* a) {
    char b[16]; int i = 0; int32_t v = a[0]; int neg = 0;
    if (v < 0) { neg = 1; v = -v; }
    if (v == 0) b[i++] = '0';
    while (v > 0 && i < 15) { b[i++] = (char)('0' + (v % 10)); v /= 10; }
    char out[18]; int j = 0;
    if (neg) out[j++] = '-';
    while (i > 0) out[j++] = b[--i];
    out[j] = 0;
    return make_string(out, j);
}

static int32_t ic_tick_count(int32_t* a) {
    (void)a;
    uint32_t t;
    __asm__ volatile("rdtsc" : "=a"(t) :: "edx");
    return (int32_t)(t >> 10);
}

struct IcallEntry { const char* name; IcallFn fn; };

// Slots for subsystems that layer on top of the CLR (the managed GUI
// shell registers ~40 of these).  Kept separate from the built-in table
// so clr.cpp never has to know what they are.
#define CLR_MAX_EXT_ICALLS 96
struct ExtIcall { const char* name; IcallFn fn; };
static ExtIcall g_ext_icalls[CLR_MAX_EXT_ICALLS];
static int      g_ext_icall_count = 0;

extern "C" int clr_register_icall(const char* fqname, clr_icall_fn fn) {
    if (g_ext_icall_count >= CLR_MAX_EXT_ICALLS) return -1;
    // Re-registering the same name replaces the handler; the GUI may be
    // torn down and rebuilt without leaking slots.
    for (int i = 0; i < g_ext_icall_count; i++) {
        if (streq_(g_ext_icalls[i].name, fqname)) {
            g_ext_icalls[i].fn = (IcallFn)fn;
            return 0;
        }
    }
    g_ext_icalls[g_ext_icall_count].name = fqname;
    g_ext_icalls[g_ext_icall_count].fn   = (IcallFn)fn;
    g_ext_icall_count++;
    return 0;
}

static const IcallEntry g_icalls[] = {
    { "NexOS.Sys::Print",      ic_print       },
    { "NexOS.Sys::PrintInt",   ic_print_int   },
    { "NexOS.Sys::PrintChar",  ic_print_char  },
    { "NexOS.Sys::StrConcat",  ic_str_concat  },
    { "NexOS.Sys::StrCharAt",  ic_str_char_at },
    { "NexOS.Sys::StrLen",     ic_str_len     },
    { "NexOS.Sys::StrEq",      ic_str_eq      },
    { "NexOS.Sys::IntToStr",   ic_int_to_str  },
    { "NexOS.Sys::TickCount",  ic_tick_count  },
};
static const int G_ICALL_COUNT = (int)(sizeof(g_icalls) / sizeof(g_icalls[0]));

// =====================================================================
//  interpreter
// =====================================================================
static inline uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline uint16_t rd16(const uint8_t* p) {
    return (uint16_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8));
}

static int exec_method(uint32_t midx, int depth);

// ---------------------------------------------------------------------
//  virtual dispatch
// ---------------------------------------------------------------------
//  The packer does not build vtables, so `callvirt` resolves by walking
//  the receiver's type chain looking for a method with the same short
//  name and arity as the statically referenced one.  That is exactly the
//  C# override rule for the subset we compile (no overloaded virtuals,
//  no explicit interface implementations).
//
//  A repaint issues thousands of callvirts per frame, so a walk over the
//  whole method table each time would be far too slow.  A direct-mapped
//  cache keyed on (token, receiver type) makes the steady state O(1).
// ---------------------------------------------------------------------
#define VCACHE_SIZE 512
struct VCacheEntry { uint32_t token, type, target; };
static VCacheEntry g_vcache[VCACHE_SIZE];
static int         g_vcache_init = 0;

static void vcache_clear(void) {
    for (int i = 0; i < VCACHE_SIZE; i++) {
        g_vcache[i].token = 0xFFFFFFFFu;
        g_vcache[i].type  = 0xFFFFFFFFu;
        g_vcache[i].target = 0;
    }
    g_vcache_init = 1;
}

static uint32_t resolve_virtual(uint32_t token, uint32_t rtype) {
    if (!g_vcache_init) vcache_clear();

    uint32_t h = (token * 31u + rtype * 131u) & (VCACHE_SIZE - 1);
    VCacheEntry* e = &g_vcache[h];
    if (e->token == token && e->type == rtype) return e->target;

    const MexMethod* base = &g_methods[token];
    const char* want = mex_name(base->short_off);
    const uint16_t arity = base->n_args;

    // Walk from the most-derived type up towards the root; the first
    // matching declaration wins, which is the override closest to the
    // dynamic type.
    uint32_t found = token;
    uint32_t t = rtype;
    int guard = 0;
    while (t < g_hdr->n_types && guard++ < 32) {
        for (uint32_t i = 0; i < g_hdr->n_methods; i++) {
            const MexMethod* c = &g_methods[i];
            if (c->decl_type != t) continue;
            if (c->n_args != arity) continue;
            if (c->flags & MF_STATIC) continue;
            if (!streq_(mex_name(c->short_off), want)) continue;
            found = i;
            goto done;
        }
        t = g_types[t].base_type;
    }
done:
    e->token = token; e->type = rtype; e->target = found;
    return found;
}

static int call_icall(const MexMethod* m, int32_t* args, int32_t* out) {
    uint32_t id = m->icall_id;
    if (id >= 128 || g_icall_bind[id] == -1) {
        char buf[128];
        scpy_(buf, "unbound internal call ", sizeof(buf));
        scat_(buf, mex_name(m->name_off), sizeof(buf));
        fault(buf);
        return -1;
    }
    int b = g_icall_bind[id];
    *out = (b >= 0) ? g_icalls[b].fn(args)
                    : g_ext_icalls[-b - 2].fn(args);
    return 0;
}

static int exec_method(uint32_t midx, int depth) {
    if (depth > CLR_MAX_DEPTH) { fault("call stack too deep"); return -1; }
    if (midx >= g_hdr->n_methods) { fault("bad method index"); return -1; }

    const MexMethod* m = &g_methods[midx];
    const int nargs = m->n_args;
    int32_t* args = &g_stack[g_sp - nargs];
    const int frame_base = g_sp - nargs;

    // ---- internal call: no IL body -----------------------------
    if (m->flags & MF_ICALL) {
        int32_t r = 0;
        if (call_icall(m, args, &r) < 0) return -1;
        g_sp = frame_base;
        if (m->flags & MF_HASRET) g_stack[g_sp++] = r;
        return 0;
    }

    if (m->code_size == 0) {
        // Abstract/interface slot, or an empty body.  An abstract slot
        // reached through a direct `call` means the packer could not
        // devirtualise it; that is a real bug, so say so loudly instead
        // of silently returning a bogus zero.
        if (m->flags & MF_ABSTRACT) {
            fault("call to abstract method");
            ser("       "); ser(mex_name(m->name_off)); ser("\n");
            return -1;
        }
        g_sp = frame_base;
        if (m->flags & MF_HASRET) g_stack[g_sp++] = 0;
        return 0;
    }

    // ---- frame layout: [args][locals][eval stack] ---------------
    int32_t* locals = &g_stack[g_sp];
    for (int i = 0; i < m->n_locals; i++) g_stack[g_sp + i] = 0;
    g_sp += m->n_locals;
    const int eval_base = g_sp;

    if (g_sp + m->max_stack + 8 >= CLR_STACK_SLOTS) {
        fault("evaluation stack overflow");
        return -1;
    }

    const uint8_t* il = g_code + m->code_off;
    const uint32_t n  = m->code_size;
    uint32_t pc = 0;

    #define PUSH(v) (g_stack[g_sp++] = (int32_t)(v))
    #define POP()   (g_stack[--g_sp])

    while (pc < n) {
        if (g_fault) return -1;
        if (++g_steps > CLR_STEP_LIMIT) { fault("step limit exceeded"); return -1; }

        uint8_t op = il[pc++];
        switch (op) {

        case 0x00: break;                                   // nop
        case 0x01: break;                                   // break

        // ---- load argument ------------------------------------
        case 0x02: case 0x03: case 0x04: case 0x05:
            PUSH(args[op - 0x02]); break;
        case 0x0E: PUSH(args[il[pc++]]); break;             // ldarg.s
        case 0x10: { uint8_t i = il[pc++]; args[i] = POP(); } break;  // starg.s

        // ---- load / store local -------------------------------
        case 0x06: case 0x07: case 0x08: case 0x09:
            PUSH(locals[op - 0x06]); break;
        case 0x0A: case 0x0B: case 0x0C: case 0x0D:
            locals[op - 0x0A] = POP(); break;
        case 0x11: PUSH(locals[il[pc++]]); break;           // ldloc.s
        case 0x13: { uint8_t i = il[pc++]; locals[i] = POP(); } break; // stloc.s

        // ---- constants ----------------------------------------
        case 0x14: PUSH(0); break;                          // ldnull
        case 0x15: PUSH(-1); break;                         // ldc.i4.m1
        case 0x16: case 0x17: case 0x18: case 0x19:
        case 0x1A: case 0x1B: case 0x1C: case 0x1D: case 0x1E:
            PUSH((int32_t)op - 0x16); break;                // ldc.i4.0..8
        case 0x1F: PUSH((int32_t)(int8_t)il[pc++]); break;  // ldc.i4.s
        case 0x20: PUSH((int32_t)rd32(il + pc)); pc += 4; break;  // ldc.i4

        case 0x25: { int32_t v = g_stack[g_sp - 1]; PUSH(v); } break;  // dup
        case 0x26: g_sp--; break;                           // pop

        // ---- calls --------------------------------------------
        case 0x28: {                                        // call
            uint32_t t = rd32(il + pc); pc += 4;
            if (exec_method(t, depth + 1) < 0) return -1;
        } break;

        case 0x6F: {                                        // callvirt
            uint32_t t = rd32(il + pc); pc += 4;
            if (t >= g_hdr->n_methods) { fault("callvirt: bad token"); return -1; }
            const MexMethod* cm = &g_methods[t];
            uint32_t target = t;
            if (cm->flags & MF_VIRTUAL) {
                int na = cm->n_args;
                if (na > 0 && g_sp >= na) {
                    uint32_t self = (uint32_t)g_stack[g_sp - na];
                    if (!self) { fault("callvirt on null reference"); return -1; }
                    uint32_t rt = *(uint32_t*)self;
                    // strings and arrays carry sentinel headers, not type ids
                    if (rt < g_hdr->n_types) target = resolve_virtual(t, rt);
                }
            }
            if (exec_method(target, depth + 1) < 0) return -1;
        } break;

        case 0x73: {                                        // newobj
            uint32_t t = rd32(il + pc); pc += 4;
            if (t >= g_hdr->n_methods) { fault("newobj: bad ctor"); return -1; }
            const MexMethod* ctor = &g_methods[t];
            uint32_t ti = ctor->decl_type;
            uint32_t sz = (ti < g_hdr->n_types) ? g_types[ti].inst_size : 4;
            if (sz < 4) sz = 4;
            uint8_t* obj = heap_alloc(sz);
            if (!obj) return -1;
            *(uint32_t*)obj = ti;
            // stack holds the ctor parameters; splice 'this' in below them
            int nparam = ctor->n_args - 1;
            if (nparam < 0) nparam = 0;
            for (int i = 0; i < nparam; i++)
                g_stack[g_sp - i] = g_stack[g_sp - i - 1];
            g_stack[g_sp - nparam] = (int32_t)(uint32_t)obj;
            g_sp++;
            if (exec_method(t, depth + 1) < 0) return -1;
            PUSH((uint32_t)obj);
        } break;

        case 0x2A: {                                        // ret
            int32_t rv = 0;
            if (m->flags & MF_HASRET) rv = POP();
            g_sp = frame_base;
            if (m->flags & MF_HASRET) g_stack[g_sp++] = rv;
            return 0;
        }

        // ---- branches -----------------------------------------
        case 0x2B: { int8_t d = (int8_t)il[pc++]; pc += d; } break;   // br.s
        case 0x2C: { int8_t d = (int8_t)il[pc++]; if (POP() == 0) pc += d; } break;
        case 0x2D: { int8_t d = (int8_t)il[pc++]; if (POP() != 0) pc += d; } break;
        case 0x2E: { int8_t d = (int8_t)il[pc++]; int32_t b=POP(),a=POP(); if (a==b) pc+=d; } break;
        case 0x2F: { int8_t d = (int8_t)il[pc++]; int32_t b=POP(),a=POP(); if (a>=b) pc+=d; } break;
        case 0x30: { int8_t d = (int8_t)il[pc++]; int32_t b=POP(),a=POP(); if (a>b)  pc+=d; } break;
        case 0x31: { int8_t d = (int8_t)il[pc++]; int32_t b=POP(),a=POP(); if (a<=b) pc+=d; } break;
        case 0x32: { int8_t d = (int8_t)il[pc++]; int32_t b=POP(),a=POP(); if (a<b)  pc+=d; } break;
        case 0x33: { int8_t d = (int8_t)il[pc++]; int32_t b=POP(),a=POP(); if (a!=b) pc+=d; } break;
        case 0x34: { int8_t d = (int8_t)il[pc++]; uint32_t b=POP(),a=POP(); if (a>=b) pc+=d; } break;
        case 0x35: { int8_t d = (int8_t)il[pc++]; uint32_t b=POP(),a=POP(); if (a>b)  pc+=d; } break;
        case 0x36: { int8_t d = (int8_t)il[pc++]; uint32_t b=POP(),a=POP(); if (a<=b) pc+=d; } break;
        case 0x37: { int8_t d = (int8_t)il[pc++]; uint32_t b=POP(),a=POP(); if (a<b)  pc+=d; } break;

        case 0x38: { int32_t d = (int32_t)rd32(il+pc); pc += 4; pc += d; } break;
        case 0x39: { int32_t d = (int32_t)rd32(il+pc); pc += 4; if (POP()==0) pc+=d; } break;
        case 0x3A: { int32_t d = (int32_t)rd32(il+pc); pc += 4; if (POP()!=0) pc+=d; } break;
        case 0x3B: { int32_t d = (int32_t)rd32(il+pc); pc += 4; int32_t b=POP(),a=POP(); if (a==b) pc+=d; } break;
        case 0x3C: { int32_t d = (int32_t)rd32(il+pc); pc += 4; int32_t b=POP(),a=POP(); if (a>=b) pc+=d; } break;
        case 0x3D: { int32_t d = (int32_t)rd32(il+pc); pc += 4; int32_t b=POP(),a=POP(); if (a>b)  pc+=d; } break;
        case 0x3E: { int32_t d = (int32_t)rd32(il+pc); pc += 4; int32_t b=POP(),a=POP(); if (a<=b) pc+=d; } break;
        case 0x3F: { int32_t d = (int32_t)rd32(il+pc); pc += 4; int32_t b=POP(),a=POP(); if (a<b)  pc+=d; } break;
        case 0x40: { int32_t d = (int32_t)rd32(il+pc); pc += 4; int32_t b=POP(),a=POP(); if (a!=b) pc+=d; } break;
        case 0x41: { int32_t d = (int32_t)rd32(il+pc); pc += 4; uint32_t b=POP(),a=POP(); if (a>=b) pc+=d; } break;
        case 0x42: { int32_t d = (int32_t)rd32(il+pc); pc += 4; uint32_t b=POP(),a=POP(); if (a>b)  pc+=d; } break;
        case 0x43: { int32_t d = (int32_t)rd32(il+pc); pc += 4; uint32_t b=POP(),a=POP(); if (a<=b) pc+=d; } break;
        case 0x44: { int32_t d = (int32_t)rd32(il+pc); pc += 4; uint32_t b=POP(),a=POP(); if (a<b)  pc+=d; } break;

        case 0x45: {                                        // switch
            uint32_t cnt = rd32(il + pc); pc += 4;
            uint32_t tblbase = pc;
            pc += 4 * cnt;
            int32_t v = POP();
            if (v >= 0 && (uint32_t)v < cnt)
                pc += (int32_t)rd32(il + tblbase + 4 * (uint32_t)v);
        } break;

        // ---- arithmetic ---------------------------------------
        case 0x58: { int32_t b=POP(),a=POP(); PUSH(a+b); } break;
        case 0x59: { int32_t b=POP(),a=POP(); PUSH(a-b); } break;
        case 0x5A: { int32_t b=POP(),a=POP(); PUSH(a*b); } break;
        case 0x5B: { int32_t b=POP(),a=POP();
                     if (!b) { fault("divide by zero"); return -1; } PUSH(a/b); } break;
        case 0x5C: { uint32_t b=POP(),a=POP();
                     if (!b) { fault("divide by zero"); return -1; } PUSH((int32_t)(a/b)); } break;
        case 0x5D: { int32_t b=POP(),a=POP();
                     if (!b) { fault("divide by zero"); return -1; } PUSH(a%b); } break;
        case 0x5E: { uint32_t b=POP(),a=POP();
                     if (!b) { fault("divide by zero"); return -1; } PUSH((int32_t)(a%b)); } break;
        case 0x5F: { int32_t b=POP(),a=POP(); PUSH(a&b); } break;
        case 0x60: { int32_t b=POP(),a=POP(); PUSH(a|b); } break;
        case 0x61: { int32_t b=POP(),a=POP(); PUSH(a^b); } break;
        case 0x62: { int32_t b=POP(),a=POP(); PUSH(a<<(b&31)); } break;
        case 0x63: { int32_t b=POP(),a=POP(); PUSH(a>>(b&31)); } break;
        case 0x64: { int32_t b=POP(); uint32_t a=POP(); PUSH((int32_t)(a>>(b&31))); } break;
        case 0x65: { int32_t a=POP(); PUSH(-a); } break;
        case 0x66: { int32_t a=POP(); PUSH(~a); } break;

        // ---- conversions (32-bit host: mostly truncation) ------
        case 0x67: { int32_t a=POP(); PUSH((int32_t)(int8_t)a); } break;   // conv.i1
        case 0x68: { int32_t a=POP(); PUSH((int32_t)(int16_t)a); } break;  // conv.i2
        case 0x69: break;                                                  // conv.i4
        case 0x6A: break;                                                  // conv.i8 (truncated)
        case 0xD1: { int32_t a=POP(); PUSH((int32_t)(uint16_t)a); } break; // conv.u2
        case 0xD2: { int32_t a=POP(); PUSH((int32_t)(uint8_t)a); } break;  // conv.u1
        case 0xD3: break;                                                  // conv.i
        case 0x82: break;                                                  // conv.u2 alias
        case 0xE0: break;                                                  // conv.u

        // ---- strings / objects --------------------------------
        case 0x72: {                                        // ldstr
            uint32_t si = rd32(il + pc); pc += 4;
            if (si >= g_hdr->n_strings || si >= CLR_MAX_STRINGS) {
                fault("ldstr: bad string index"); return -1;
            }
            PUSH(g_strobj[si]);
        } break;

        case 0x7B: {                                        // ldfld
            uint32_t off = rd32(il + pc); pc += 4;
            uint32_t o = (uint32_t)POP();
            if (!o) { fault("null reference in ldfld"); return -1; }
            PUSH(*(int32_t*)(o + off));
        } break;
        case 0x7D: {                                        // stfld
            uint32_t off = rd32(il + pc); pc += 4;
            int32_t v = POP();
            uint32_t o = (uint32_t)POP();
            if (!o) { fault("null reference in stfld"); return -1; }
            *(int32_t*)(o + off) = v;
        } break;
        case 0x7E: {                                        // ldsfld
            uint32_t s = rd32(il + pc); pc += 4;
            PUSH(s < CLR_MAX_STATICS ? g_statics[s] : 0);
        } break;
        case 0x80: {                                        // stsfld
            uint32_t s = rd32(il + pc); pc += 4;
            int32_t v = POP();
            if (s < CLR_MAX_STATICS) g_statics[s] = v;
        } break;

        case 0x8C: {                                        // box
            uint32_t ti = rd32(il + pc); pc += 4;
            int32_t v = POP();
            uint8_t* o = heap_alloc(8);
            if (!o) return -1;
            *(uint32_t*)o = ti;
            *(int32_t*)(o + 4) = v;
            PUSH((uint32_t)o);
        } break;
        case 0xA5: case 0x79: {                             // unbox.any / unbox
            pc += 4;
            uint32_t o = (uint32_t)POP();
            if (!o) { fault("null reference in unbox"); return -1; }
            PUSH(*(int32_t*)(o + 4));
        } break;
        case 0x74: pc += 4; break;                          // castclass (unchecked)
        case 0x75: pc += 4; break;                          // isinst    (unchecked)

        // ---- arrays -------------------------------------------
        case 0x8D: {                                        // newarr
            pc += 4;
            int32_t cnt = POP();
            if (cnt < 0) { fault("negative array size"); return -1; }
            uint8_t* a = heap_alloc(12 + (uint32_t)cnt * 4);
            if (!a) return -1;
            *(uint32_t*)a = OBJ_ARRAY;
            *(uint32_t*)(a + 4) = (uint32_t)cnt;
            *(uint32_t*)(a + 8) = 4;
            PUSH((uint32_t)a);
        } break;
        case 0x8E: {                                        // ldlen
            uint32_t a = (uint32_t)POP();
            if (!a) { fault("null reference in ldlen"); return -1; }
            PUSH(*(uint32_t*)(a + 4));
        } break;
        case 0x91: case 0x92: case 0x93: case 0x94:
        case 0x95: case 0x96: case 0x9A: {                  // ldelem.*
            int32_t i = POP(); uint32_t a = (uint32_t)POP();
            if (!a) { fault("null reference in ldelem"); return -1; }
            if (i < 0 || (uint32_t)i >= *(uint32_t*)(a + 4)) {
                fault("array index out of range"); return -1;
            }
            PUSH(*(int32_t*)(a + 12 + (uint32_t)i * 4));
        } break;
        case 0x9C: case 0x9D: case 0x9E: case 0xA2: {       // stelem.*
            int32_t v = POP(); int32_t i = POP(); uint32_t a = (uint32_t)POP();
            if (!a) { fault("null reference in stelem"); return -1; }
            if (i < 0 || (uint32_t)i >= *(uint32_t*)(a + 4)) {
                fault("array index out of range"); return -1;
            }
            *(int32_t*)(a + 12 + (uint32_t)i * 4) = v;
        } break;
        case 0xA3: {                                        // ldelem <type>
            pc += 4;
            int32_t i = POP(); uint32_t a = (uint32_t)POP();
            if (!a) { fault("null reference in ldelem"); return -1; }
            PUSH(*(int32_t*)(a + 12 + (uint32_t)i * 4));
        } break;
        case 0xA4: {                                        // stelem <type>
            pc += 4;
            int32_t v = POP(); int32_t i = POP(); uint32_t a = (uint32_t)POP();
            if (!a) { fault("null reference in stelem"); return -1; }
            *(int32_t*)(a + 12 + (uint32_t)i * 4) = v;
        } break;

        case 0x7A: fault("managed exception thrown"); return -1;   // throw

        // ---- two byte opcodes ---------------------------------
        case 0xFE: {
            uint8_t op2 = il[pc++];
            switch (op2) {
            case 0x01: { int32_t b=POP(),a=POP(); PUSH(a==b?1:0); } break;  // ceq
            case 0x02: { int32_t b=POP(),a=POP(); PUSH(a>b?1:0); } break;   // cgt
            case 0x03: { uint32_t b=POP(),a=POP(); PUSH(a>b?1:0); } break;  // cgt.un
            case 0x04: { int32_t b=POP(),a=POP(); PUSH(a<b?1:0); } break;   // clt
            case 0x05: { uint32_t b=POP(),a=POP(); PUSH(a<b?1:0); } break;  // clt.un
            case 0x09: PUSH(args[rd16(il + pc)]); pc += 2; break;           // ldarg
            case 0x0B: { uint16_t i = rd16(il + pc); pc += 2; args[i] = POP(); } break;
            case 0x0C: PUSH(locals[rd16(il + pc)]); pc += 2; break;         // ldloc
            case 0x0E: { uint16_t i = rd16(il + pc); pc += 2; locals[i] = POP(); } break;
            case 0x15: {                                                    // initobj
                pc += 4;
                uint32_t o = (uint32_t)POP();
                if (o) *(int32_t*)o = 0;
            } break;
            case 0x16: pc += 4; break;                                      // constrained.
            case 0x13: break;                                               // volatile.
            case 0x14: break;                                               // tail.
            case 0x1C: { pc += 4; PUSH(4); } break;                         // sizeof
            default: {
                char b[64];
                scpy_(b, "unsupported IL opcode 0xFE ", sizeof(b));
                const char* hx = "0123456789ABCDEF";
                char h[3] = { hx[(op2 >> 4) & 0xF], hx[op2 & 0xF], 0 };
                scat_(b, h, sizeof(b));
                fault(b);
                return -1;
            }
            }
        } break;

        default: {
            char b[64];
            scpy_(b, "unsupported IL opcode 0x", sizeof(b));
            const char* hx = "0123456789ABCDEF";
            char h[3] = { hx[(op >> 4) & 0xF], hx[op & 0xF], 0 };
            scat_(b, h, sizeof(b));
            fault(b);
            return -1;
        }
        }
    }

    // fell off the end without ret
    (void)eval_base;
    g_sp = frame_base;
    if (m->flags & MF_HASRET) g_stack[g_sp++] = 0;
    return 0;

    #undef PUSH
    #undef POP
}

// =====================================================================
//  image loading
// =====================================================================
static int load_image(const char* filename) {
    if (!g_reader) { fault("no file reader bound"); return -1; }
    if (!g_image) {
        g_image = (uint8_t*)kmalloc(CLR_IMAGE_MAX);
        if (!g_image) { fault("cannot allocate image buffer"); return -3; }
    }
    int got = g_reader(filename, g_image, CLR_IMAGE_MAX);
    if (got <= 0) {
        scpy_(g_report, "CLR: file not found: ", sizeof(g_report));
        scat_(g_report, filename, sizeof(g_report));
        return -1;
    }
    if (got < (int)sizeof(MexHeader)) { fault("image too small"); return -2; }

    MexHeader* h = (MexHeader*)g_image;
    if (h->magic[0] != 'M' || h->magic[1] != 'E' ||
        h->magic[2] != 'X' || h->magic[3] != '1') {
        fault("not a MEX1 image"); return -2;
    }
    if (h->version != 1) { fault("unsupported .mex version"); return -2; }
    if (h->off_code + h->code_size > (uint32_t)got) {
        fault("truncated image"); return -2;
    }

    g_hdr     = h;
    g_methods = (MexMethod*)(g_image + h->off_methods);
    g_types   = (MexType*)(g_image + h->off_types);
    g_strtab  = (MexStr*)(g_image + h->off_strings);
    g_code    = g_image + h->off_code;
    g_names   = (const char*)(g_image + h->off_names);
    g_litdata = g_image + h->off_litdata;
    vcache_clear();          // stale entries would point into the old image

    if (h->n_statics > CLR_MAX_STATICS) { fault("too many static fields"); return -2; }
    if (h->n_strings > CLR_MAX_STRINGS) { fault("too many string literals"); return -2; }
    if (h->n_icalls  > 128)             { fault("too many internal calls"); return -2; }

    // ---- bind internal calls by name ---------------------------
    const uint32_t* icoffs = (const uint32_t*)(g_image + h->off_icalls);
    int unbound = 0;
    for (uint32_t i = 0; i < h->n_icalls; i++) {
        const char* want = mex_name(icoffs[i]);
        g_icall_bind[i] = -1;
        for (int j = 0; j < G_ICALL_COUNT; j++) {
            if (streq_(g_icalls[j].name, want)) { g_icall_bind[i] = (int16_t)j; break; }
        }
        if (g_icall_bind[i] < 0) {
            // Not a built-in: look through the subsystem registrations.
            // Encoded as -(2+index) so 0.. stays the built-in range and
            // -1 keeps meaning "unbound".
            for (int j = 0; j < g_ext_icall_count; j++) {
                if (streq_(g_ext_icalls[j].name, want)) {
                    g_icall_bind[i] = (int16_t)(-(2 + j));
                    break;
                }
            }
        }
        if (g_icall_bind[i] == -1) {
            unbound++;
            ser("[CLR] unbound icall: "); ser(want); ser("\n");
        }
    }

    // ---- managed heap ------------------------------------------
    if (!g_heap) {
        g_heap = (uint8_t*)kmalloc(CLR_HEAP_SIZE);
        if (!g_heap) { fault("cannot allocate managed heap"); return -3; }
    }
    g_heap_used = 0;

    // ---- materialise string literals ---------------------------
    for (uint32_t i = 0; i < h->n_strings; i++) {
        g_strobj[i] = make_string((const char*)(g_litdata + g_strtab[i].off),
                                  (int)g_strtab[i].len);
    }
    for (uint32_t i = 0; i < h->n_statics; i++) g_statics[i] = 0;

    ser("[CLR] loaded "); ser(filename);
    ser("  methods="); ser_int((int32_t)h->n_methods);
    ser(" types=");    ser_int((int32_t)h->n_types);
    ser(" strings=");  ser_int((int32_t)h->n_strings);
    ser(" code=");     ser_int((int32_t)h->code_size);
    if (unbound) { ser(" unbound_icalls="); ser_int(unbound); }
    ser("\n");
    return 0;
}

// =====================================================================
//  public API
// =====================================================================
extern "C" void clr_init(clr_read_fn reader) {
    g_reader = reader;
    g_image = 0;
    g_heap = 0;
    g_heap_used = 0;
    g_sp = 0;
    g_fault = 0;
    scpy_(g_report, "MiniCLR ready", sizeof(g_report));
    ser("[CLR] MiniCLR initialised (");
    ser_int(G_ICALL_COUNT);
    ser(" internal calls)\n");
}

extern "C" int clr_ready(void) { return g_reader != 0; }

extern "C" const char* clr_str(int32_t obj) {
    if (!obj) return "";
    return str_data(obj);
}

extern "C" int32_t clr_new_str(const char* s) {
    if (!s) s = "";
    return make_string(s, slen_(s));
}

extern "C" const char* clr_last_report(void) { return g_report; }

extern "C" int clr_run(const char* filename) {
    g_fault = 0;
    g_steps = 0;
    g_sp = 0;
    scpy_(g_report, "", sizeof(g_report));

    int rc = load_image(filename);
    if (rc != 0) {
        if (!g_report[0]) scpy_(g_report, "CLR: load failed", sizeof(g_report));
        return rc;
    }

    uint32_t entry = g_hdr->entry;
    if (entry >= g_hdr->n_methods) { fault("bad entry point"); return -2; }

    ser("[CLR] entry "); ser(mex_name(g_methods[entry].name_off)); ser("\n");

    const MexMethod* em = &g_methods[entry];
    for (int i = 0; i < em->n_args; i++) g_stack[g_sp++] = 0;

    if (exec_method(entry, 0) < 0) return -5;

    if (g_fault) return -5;

    scpy_(g_report, "CLR: ", sizeof(g_report));
    scat_(g_report, filename, sizeof(g_report));
    scat_(g_report, " exited normally", sizeof(g_report));
    ser("[CLR] "); ser(filename); ser(" exited normally, heap used ");
    ser_int((int32_t)g_heap_used); ser(" bytes\n");
    return 0;
}

// =====================================================================
//  Resident hosting API  -- the managed GUI shell lives here
// =====================================================================
static int g_resident = 0;

extern "C" int clr_load(const char* filename) {
    g_fault = 0;
    g_steps = 0;
    g_sp = 0;
    g_resident = 0;
    scpy_(g_report, "", sizeof(g_report));

    int rc = load_image(filename);
    if (rc != 0) {
        if (!g_report[0]) scpy_(g_report, "CLR: load failed", sizeof(g_report));
        return rc;
    }
    g_resident = 1;
    scpy_(g_report, "CLR: ", sizeof(g_report));
    scat_(g_report, filename, sizeof(g_report));
    scat_(g_report, " resident", sizeof(g_report));
    return 0;
}

extern "C" int clr_loaded(void) { return g_resident && !g_fault; }

extern "C" void clr_unload(void) { g_resident = 0; }

extern "C" uint32_t clr_heap_mark(void) { return g_heap_used; }

extern "C" void clr_heap_reset(uint32_t mark) {
    if (mark <= g_heap_used) g_heap_used = mark;
}

extern "C" uint32_t clr_heap_used(void) { return g_heap_used; }

// Locate a static method by "Type::Method".  Linear scan is fine: the
// shell resolves each entry point once and caches the index.
static int find_method(const char* fq) {
    if (!g_resident) return -1;
    for (uint32_t i = 0; i < g_hdr->n_methods; i++) {
        if (streq_(mex_name(g_methods[i].name_off), fq)) return (int)i;
    }
    return -1;
}

extern "C" int clr_call(const char* fqname, const int32_t* args,
                        int nargs, int32_t* ret) {
    if (ret) *ret = 0;
    if (!g_resident) { scpy_(g_report, "CLR: no image loaded", sizeof(g_report)); return -1; }

    int mi = find_method(fqname);
    if (mi < 0) {
        scpy_(g_report, "CLR: method not found: ", sizeof(g_report));
        scat_(g_report, fqname, sizeof(g_report));
        return -1;
    }

    const MexMethod* m = &g_methods[mi];
    if (nargs != m->n_args) {
        scpy_(g_report, "CLR: argument count mismatch on ", sizeof(g_report));
        scat_(g_report, fqname, sizeof(g_report));
        return -1;
    }

    // A fault inside one callback must not poison every later frame, so
    // the fault flag is cleared per call.  g_report keeps the message.
    g_fault = 0;
    g_steps = 0;
    g_sp = 0;
    for (int i = 0; i < nargs; i++) g_stack[g_sp++] = args[i];

    if (exec_method((uint32_t)mi, 0) < 0 || g_fault) {
        g_resident = 0;              // stop calling into a broken image
        return -5;
    }
    if (ret && (m->flags & MF_HASRET) && g_sp > 0) *ret = g_stack[g_sp - 1];
    return 0;
}
