/* =====================================================================
 *  utils_smoke.c - Phase 0.4 smoke test for Mono's utils layer
 * ---------------------------------------------------------------------
 *  `ar` succeeding proves nothing: a Mono source file that is entirely
 *  wrapped in an #ifdef we never enabled still produces a valid, empty
 *  object, and a file that compiles can still reference symbols that do
 *  not exist anywhere in our five libraries.  `make audit` catches the
 *  second case statically; this test catches both dynamically, by
 *  actually linking against libmono_utils.a and running the code.
 *
 *  Scope is the self-contained subset of utils/ -- utils/ is NOT a
 *  bottom layer.  utils/mono-error.c constructs MonoException objects and
 *  utils/mono-threads-posix.c calls mono_gc_pthread_create, both of which
 *  live in metadata/, so anything reaching mono-threads or hazard-pointer
 *  cannot be linked until metadata/ and mini/ are in.  Upstream never
 *  trips over this because it all lands in one libmono.so.
 *  `bash tools/leafcheck.sh` computes the linkable subset: 57 of 83
 *  objects today.  Everything tested below is in it, and is something the
 *  interpreter genuinely leans on:
 *
 *    monobitset          liveness / verifier bit vectors
 *    mono-digest         assembly hashes (MD5, SHA1)
 *    memfuncs            the GC's word-at-a-time bzero/memmove
 *    mono-math-c         the classification helpers behind conv.* opcodes
 *    mono-path           assembly path canonicalisation
 *    mono-internal-hash  the intrusive hash behind MonoImage->class_cache
 *
 *  Digests are checked against the canonical RFC/FIPS "abc" vectors
 *  rather than against ourselves, so a byte-order or padding bug in the
 *  32-bit freestanding build cannot pass by agreeing with itself.
 *
 *  Console/exit go through `int 0x80` exactly as in eglib_smoke.c, so
 *  this binary runs both on the WSL host and, unchanged, as a MiniOS
 *  ring-3 process.
 * ===================================================================== */

#include <glib.h>
#include <mono/utils/monobitset.h>
#include <mono/utils/mono-digest.h>
/* mono-math.h has two faces (mono-math.h:20 vs :74): from C++ it declares
   mono_isnan_double() and friends, from plain C it #defines mono_isnan to
   libm's isnan.  We want the real functions out of mono-math-c.o, not a
   libm we do not have, so we ask for the explicit declarations the same
   way upstream's C callers do. */
#define MONO_MATH_DECLARE_ALL 1
#include <mono/utils/mono-math.h>
#include <mono/utils/mono-path.h>
#include <mono/utils/mono-internal-hash.h>

extern void (*pal_console_putc)(char);

void mono_gc_bzero_aligned   (void *dest, size_t size);
void mono_gc_memmove_aligned (void *dest, const void *src, size_t size);

/* ---- raw write(2) / exit(2) via int 0x80 (no libc) ---- */
static int sys_write(int fd, const void *buf, int len)
{
    int r;
    __asm__ __volatile__("int $0x80"
                         : "=a"(r)
                         : "a"(4), "b"(fd), "c"(buf), "d"(len)
                         : "memory");
    return r;
}
static void sys_exit(int code)
{
    __asm__ __volatile__("int $0x80" :: "a"(1), "b"(code));
    for (;;) { }
}

static void con_putc(char c) { sys_write(1, &c, 1); }
static void out(const char *s) { while (*s) con_putc(*s++); }
static void out_num(long v)
{
    char b[24]; int i = 0;
    if (v < 0) { con_putc('-'); v = -v; }
    if (!v) { con_putc('0'); return; }
    while (v) { b[i++] = (char)('0' + (v % 10)); v /= 10; }
    while (i) con_putc(b[--i]);
}

static int g_fails = 0;
static void check(const char *what, int ok)
{
    out(ok ? "  [ok]   " : "  [FAIL] ");
    out(what);
    con_putc('\n');
    if (!ok) g_fails++;
}

/* hex compare against a lowercase reference string */
static int digest_is(const unsigned char *d, int n, const char *hex)
{
    static const char *H = "0123456789abcdef";
    for (int i = 0; i < n; i++) {
        if (hex[i * 2]     != H[(d[i] >> 4) & 0xF]) return 0;
        if (hex[i * 2 + 1] != H[ d[i]       & 0xF]) return 0;
    }
    return hex[n * 2] == '\0';
}

/* ------------------------------------------------------------------ */
static void test_bitset(void)
{
    /* 200 bits spans several chunks on a 32-bit target, so the
       chunk-crossing paths in find_first/count actually get exercised. */
    MonoBitSet *bs = mono_bitset_new(200, 0);
    check("bitset new", bs != NULL && mono_bitset_size(bs) >= 200);
    if (!bs) return;

    mono_bitset_clear_all(bs);
    check("bitset clear_all", mono_bitset_count(bs) == 0);

    mono_bitset_set(bs, 0);
    mono_bitset_set(bs, 31);      /* last bit of chunk 0 */
    mono_bitset_set(bs, 32);      /* first bit of chunk 1 */
    mono_bitset_set(bs, 199);     /* last bit overall     */
    check("bitset count", mono_bitset_count(bs) == 4);
    check("bitset test set",   mono_bitset_test(bs, 32) && mono_bitset_test(bs, 199));
    check("bitset test unset", !mono_bitset_test(bs, 33) && !mono_bitset_test(bs, 100));

    mono_bitset_clear(bs, 0);
    check("bitset find_first", mono_bitset_find_first(bs, -1) == 31);

    MonoBitSet *cl = mono_bitset_clone(bs, 0);
    check("bitset clone", cl && mono_bitset_count(cl) == 3 && mono_bitset_test(cl, 199));
    if (cl) mono_bitset_free(cl);

    mono_bitset_set_all(bs);
    check("bitset set_all", mono_bitset_count(bs) == mono_bitset_size(bs));
    mono_bitset_free(bs);
}

static void test_digest(void)
{
    const unsigned char abc[3] = { 'a', 'b', 'c' };
    unsigned char md5[16], sha1[20];

    mono_md5_get_digest(abc, 3, md5);
    check("md5(\"abc\") == RFC 1321 vector",
          digest_is(md5, 16, "900150983cd24fb0d6963f7d28e17f72"));

    mono_sha1_get_digest(abc, 3, sha1);
    check("sha1(\"abc\") == FIPS 180-1 vector",
          digest_is(sha1, 20, "a9993e364706816aba3e25717850c26c9cd0d89d"));

    /* Incremental path must agree with the one-shot path: the interpreter
       hashes assemblies in chunks, not in one buffer. */
    MonoMD5Context ctx;
    unsigned char md5b[16];
    mono_md5_init(&ctx);
    mono_md5_update(&ctx, (const unsigned char *)"a",  1);
    mono_md5_update(&ctx, (const unsigned char *)"bc", 2);
    mono_md5_final(&ctx, md5b);
    check("md5 incremental == one-shot", memcmp(md5, md5b, 16) == 0);
}

static void test_memfuncs(void)
{
    /* aligned: these are the GC's own word-at-a-time routines, not libc */
    static gsize buf[64];
    for (int i = 0; i < 64; i++) buf[i] = (gsize)(i + 1);

    mono_gc_bzero_aligned(&buf[16], 16 * sizeof(gsize));
    int zeroed = 1, kept = 1;
    for (int i = 16; i < 32; i++) if (buf[i] != 0) zeroed = 0;
    for (int i = 0; i < 16; i++)  if (buf[i] != (gsize)(i + 1)) kept = 0;
    for (int i = 32; i < 64; i++) if (buf[i] != (gsize)(i + 1)) kept = 0;
    check("gc_bzero_aligned zeroes range", zeroed);
    check("gc_bzero_aligned spares neighbours", kept);

    mono_gc_memmove_aligned(&buf[16], &buf[0], 16 * sizeof(gsize));
    int moved = 1;
    for (int i = 0; i < 16; i++) if (buf[16 + i] != (gsize)(i + 1)) moved = 0;
    check("gc_memmove_aligned copies", moved);

    /* overlapping, destination above source - the direction that a naive
       forward copy gets wrong */
    mono_gc_memmove_aligned(&buf[4], &buf[0], 8 * sizeof(gsize));
    int overlap = 1;
    for (int i = 0; i < 8; i++) if (buf[4 + i] != (gsize)(i + 1)) overlap = 0;
    check("gc_memmove_aligned handles overlap", overlap);
}

/* mono-math-c.c exists precisely because these cannot be assumed from a
   hosted libm.  The interpreter's conv.ovf.* and ckfinite opcodes are
   built on them, so a wrong answer here is a silently wrong program. */
static void test_math(void)
{
    /* Construct the specials bit-wise: writing 0.0/0.0 would let the
       compiler fold it at -O0 and prove nothing about the runtime code. */
    union { guint64 u; double d; } qnan = { 0x7FF8000000000000ULL },
                                   pinf = { 0x7FF0000000000000ULL },
                                   ninf = { 0xFFF0000000000000ULL },
                                   nzer = { 0x8000000000000000ULL };
    double one = 1.0;

    check("isnan(NaN)",        mono_isnan_double(qnan.d) != 0);
    check("!isnan(1.0)",       mono_isnan_double(one)    == 0);
    check("isinf(+Inf)",       mono_isinf_double(pinf.d) != 0);
    check("isinf(-Inf)",       mono_isinf_double(ninf.d) != 0);
    check("!isfinite(+Inf)",   mono_isfinite_double(pinf.d) == 0);
    check("isfinite(1.0)",     mono_isfinite_double(one)    != 0);
    /* -0.0 is the classic trap: it compares equal to 0.0, so only signbit
       can tell them apart, and the interpreter needs that for conv/rem. */
    check("signbit(-0.0)",     mono_signbit_double(nzer.d) != 0);
    check("!signbit(+0.0)",    mono_signbit_double(0.0)    == 0);
    check("isunordered(NaN,1)", mono_isunordered_double(qnan.d, one) != 0);
    check("trunc(-2.7) == -2", mono_trunc_double(-2.7) == -2.0);
    check("trunc(2.7)  ==  2", mono_trunc_double(2.7)  ==  2.0);

    /* the float overloads are a separate code path, not a cast wrapper */
    union { guint32 u; float f; } fnan = { 0x7FC00000u },
                                  finf = { 0x7F800000u };
    check("isnan(NaN) float",  mono_isnan_float(fnan.f)  != 0);
    check("isinf(Inf) float",  mono_isinf_float(finf.f)  != 0);
    check("trunc(-2.7f) float", mono_trunc_float(-2.7f) == -2.0f);
}

static void test_path(void)
{
    /* mono_path_canonicalize is what turns the assembly search paths into
       something the VFS can look up. */
    char *p = mono_path_canonicalize("/lib/mono/./4.5/../2.0/mscorlib.dll");
    check("path canonicalize",
          p && strcmp(p, "/lib/mono/2.0/mscorlib.dll") == 0);
    if (p) g_free(p);

    char *q = mono_path_canonicalize("/a//b///c");
    check("path collapses slashes", q && strcmp(q, "/a/b/c") == 0);
    if (q) g_free(q);
}

/* --- intrusive hash: the value carries its own chain pointer --------- */
typedef struct _HItem { const char *name; struct _HItem *next; } HItem;
static gpointer  h_key  (gpointer v) { return (gpointer)((HItem *)v)->name; }
static gpointer *h_next (gpointer v) { return (gpointer *)&((HItem *)v)->next; }

static void test_internal_hash(void)
{
    static MonoInternalHashTable t;
    static HItem items[4] = {
        { "System.Object", NULL }, { "System.String", NULL },
        { "System.Int32",  NULL }, { "System.Array",  NULL },
    };

    mono_internal_hash_table_init(&t, g_str_hash, h_key, h_next);
    for (int i = 0; i < 4; i++)
        mono_internal_hash_table_insert(&t, (gpointer)items[i].name, &items[i]);

    HItem *f = (HItem *)mono_internal_hash_table_lookup(&t, (gpointer)"System.Int32");
    check("internal hash lookup hit",  f == &items[2]);
    check("internal hash lookup miss",
          mono_internal_hash_table_lookup(&t, (gpointer)"System.Nope") == NULL);
    check("internal hash remove",
          mono_internal_hash_table_remove(&t, (gpointer)"System.String"));
    check("internal hash removed gone",
          mono_internal_hash_table_lookup(&t, (gpointer)"System.String") == NULL);
    check("internal hash others intact",
          mono_internal_hash_table_lookup(&t, (gpointer)"System.Array") == &items[3]);
    mono_internal_hash_table_destroy(&t);
}

int _start_c(void)
{
    pal_console_putc = con_putc;

    out("=== mono utils freestanding smoke (mono_port Phase 0.4) ===\n");
    test_bitset();
    test_digest();
    test_memfuncs();
    test_math();
    test_path();
    test_internal_hash();

    out("=== ");
    if (g_fails == 0) out("ALL PASS");
    else { out("FAILURES: "); out_num(g_fails); }
    out(" ===\n");
    return g_fails;
}

/* no libc -> supply the ELF entry point ourselves */
__asm__(".globl _start\n_start:\n\tcall _start_c\n\tmovl %eax, %ebx\n\tmovl $1, %eax\n\tint $0x80\n");

void pal_utils_smoke_keep_exit(void) { sys_exit(0); }
