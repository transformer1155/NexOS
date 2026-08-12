/* =====================================================================
 *  libc_impl.c - Minimal freestanding libc backed by MiniOS PAL
 * ---------------------------------------------------------------------
 *  Provides the few libc symbols eglib's platform-generic core needs,
 *  so it can compile & run under -m32 -ffreestanding with no host libc.
 *  Memory comes from a statically reserved bump heap (no syscall yet);
 *  console output is optional (set pal_console_putc).  For Phase 0 the
 *  goal is: compile eglib freestanding -> libmono_port.a -> link a ring-3
 *  test that calls eglib and prints via the MiniOS serial syscall.
 * ===================================================================== */

#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>      /* PAL shim: FILE, pal_stdout/... */
#include <stdlib.h>     /* PAL shim: div_t/ldiv_t/lldiv_t   */
#include <time.h>       /* PAL shim: time_t, struct timeval */

/* ---- console hook (set by the ring-3 harness) ---- */
void (*pal_console_putc)(char) = 0;

/* ---- errno ---- */
int pal_errno = 0;

/* ---- standard streams -------------------------------------------------
 * We have no real FILE machinery; every stream funnels into
 * pal_console_putc.  The objects only need distinct addresses so that
 * callers can compare/pass them around.
 */
struct PAL_FILE { int fd; };
static struct PAL_FILE g_stdin_obj  = { 0 };
static struct PAL_FILE g_stdout_obj = { 1 };
static struct PAL_FILE g_stderr_obj = { 2 };
FILE *pal_stdin  = &g_stdin_obj;
FILE *pal_stdout = &g_stdout_obj;
FILE *pal_stderr = &g_stderr_obj;

/* ---- bump allocator over a reserved 4 MiB heap ------------------------
 * Each block carries a 16-byte header holding its requested size, which is
 * what makes a *correct* realloc possible (eglib's GArray/GString/GPtrArray
 * grow exclusively through g_realloc -- a realloc that does not preserve
 * the old contents silently corrupts every dynamic container).
 * The top-most block can also grow in place, which keeps the doubling
 * growth pattern from burning through the heap.
 */
#define HEAP_PAGES (1024u)
static unsigned char g_heap[HEAP_PAGES * 4096u] __attribute__((aligned(16)));
static unsigned long g_heap_off = 0;

typedef struct { size_t size; size_t _pad[3]; } blk_hdr_t;   /* 16 bytes on x86-32 */

#define ALIGN16(n) (((n) + 15u) & ~((size_t)15u))

void *malloc(size_t size){
    size_t slot = ALIGN16(size ? size : 1);
    if (g_heap_off + sizeof(blk_hdr_t) + slot > sizeof(g_heap)) return 0;
    blk_hdr_t *h = (blk_hdr_t *)&g_heap[g_heap_off];
    g_heap_off += sizeof(blk_hdr_t) + slot;
    h->size = size;
    return (void *)(h + 1);
}
void free(void *ptr){ (void)ptr; /* bump allocator: no reclaim in Phase 0 */ }

void *calloc(size_t nmemb, size_t size){
    size_t total = nmemb * size;
    void *p = malloc(total);
    if (p) { unsigned char *q = (unsigned char *)p; for (size_t i = 0; i < total; i++) q[i] = 0; }
    return p;
}

void *realloc(void *ptr, size_t size){
    if (!ptr)  return malloc(size);
    if (!size) { free(ptr); return 0; }

    blk_hdr_t *h    = ((blk_hdr_t *)ptr) - 1;
    size_t     slot = ALIGN16(h->size ? h->size : 1);

    /* already big enough for the new request */
    if (size <= slot) { h->size = size; return ptr; }

    /* top-most block: extend in place instead of re-allocating */
    if ((unsigned char *)h + sizeof(blk_hdr_t) + slot == &g_heap[g_heap_off]) {
        size_t nslot = ALIGN16(size);
        if (g_heap_off - slot + nslot <= sizeof(g_heap)) {
            g_heap_off = g_heap_off - slot + nslot;
            h->size = size;
            return ptr;
        }
        return 0;
    }

    void *n = malloc(size);
    if (!n) return 0;
    {   /* preserve the old contents -- the whole point of realloc */
        const unsigned char *s = (const unsigned char *)ptr;
        unsigned char       *d = (unsigned char *)n;
        size_t               c = h->size < size ? h->size : size;
        for (size_t i = 0; i < c; i++) d[i] = s[i];
    }
    return n;
}

/* ---- memory / string builtins (simple, compiler may override) ---- */
void *memcpy(void *d, const void *s, size_t n){
    unsigned char *a=(unsigned char*)d; const unsigned char *b=(const unsigned char*)s;
    for (size_t i=0;i<n;i++) a[i]=b[i];
    return d;
}
void *memmove(void *d, const void *s, size_t n){
    unsigned char *a=(unsigned char*)d; const unsigned char *b=(const unsigned char*)s;
    if (a < b) for (size_t i=0;i<n;i++) a[i]=b[i];
    else for (size_t i=n;i>0;i--) a[i-1]=b[i-1];
    return d;
}
void *memset(void *s, int c, size_t n){
    unsigned char *a=(unsigned char*)s; unsigned char v=(unsigned char)c;
    for (size_t i=0;i<n;i++) a[i]=v;
    return s;
}
int memcmp(const void *s1, const void *s2, size_t n){
    const unsigned char *a=(const unsigned char*)s1, *b=(const unsigned char*)s2;
    for (size_t i=0;i<n;i++){ if (a[i]!=b[i]) return (int)a[i]-(int)b[i]; }
    return 0;
}
void *memchr(const void *s, int c, size_t n){
    const unsigned char *a=(const unsigned char*)s;
    for (size_t i=0;i<n;i++) if (a[i]==(unsigned char)c) return (void*)&a[i];
    return 0;
}
size_t strlen(const char *s){ size_t n=0; while (s[n]) n++; return n; }
int strcmp(const char *a, const char *b){
    while (*a && *a==*b){ a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}
int strncmp(const char *a, const char *b, size_t n){
    for (size_t i=0;i<n;i++){ if (!*a||*a!=*b) return (int)(unsigned char)*a-(int)(unsigned char)*b; a++; b++; }
    return 0;
}
static int tolower_(int c){ return (c>='A'&&c<='Z')?c-'A'+'a':c; }
int strcasecmp(const char *a, const char *b){
    int d = 0;
    while (*a && (d=tolower_((unsigned char)*a)-tolower_((unsigned char)*b))==0){ a++; b++; }
    if (d == 0) d = tolower_((unsigned char)*a)-tolower_((unsigned char)*b);
    return d;
}
int strncasecmp(const char *a, const char *b, size_t n){
    int d=0;
    for (size_t i=0;i<n;i++){
        d = tolower_((unsigned char)*a)-tolower_((unsigned char)*b);
        if (d || !*a) break;
        a++; b++;
    }
    return d;
}
char *strcpy(char *d, const char *s){ char *r=d; while ((*d++=*s++)); return r; }
char *strncpy(char *d, const char *s, size_t n){
    char *r=d; size_t i=0;
    for (; i<n && s[i]; i++) d[i]=s[i];
    for (; i<n; i++) d[i]=0;
    return r;
}
char *strcat(char *d, const char *s){ return strcpy(d+strlen(d), s); }
char *strncat(char *d, const char *s, size_t n){
    size_t l=strlen(d); size_t i=0;
    for (; i<n && s[i]; i++) d[l+i]=s[i];
    d[l+i]=0;
    return d;
}
char *strdup(const char *s){ size_t n=strlen(s)+1; char *p=malloc(n); if (p) memcpy(p,s,n); return p; }
char *strndup(const char *s, size_t n){ size_t l=0; while (l<n && s[l]) l++; char *p=malloc(l+1); if (p){ memcpy(p,s,l); p[l]=0; } return p; }
char *strchr(const char *s, int c){ for (;; s++){ if (*s==(char)c) return (char*)s; if (!*s) return 0; } }
char *strrchr(const char *s, int c){ const char *r=0; for (;; s++){ if (*s==(char)c) r=s; if (!*s) return (char*)r; } }
char *strstr(const char *h, const char *n){
    if (!*n) return (char*)h;
    for (; *h; h++){ const char *a=h,*b=n; while (*a&&*b&&*a==*b){a++;b++;} if (!*b) return (char*)h; }
    return 0;
}
char *strerror(int e){ (void)e; return (char*)"errno"; }
int strerror_r(int e, char *buf, size_t n){
    (void)e;
    const char *m = "errno";
    size_t i = 0;
    if (!buf || !n) return 0;
    for (; m[i] && i + 1 < n; i++) buf[i] = m[i];
    buf[i] = 0;
    return 0;
}
static int in_set_(char c, const char *set){ while (*set){ if (*set==c) return 1; set++; } return 0; }
size_t strspn(const char *s, const char *accept){
    size_t n=0; while (s[n] && in_set_(s[n], accept)) n++; return n;
}
size_t strcspn(const char *s, const char *reject){
    size_t n=0; while (s[n] && !in_set_(s[n], reject)) n++; return n;
}
char *strpbrk(const char *s, const char *accept){
    for (; *s; s++) if (in_set_(*s, accept)) return (char*)s;
    return 0;
}
char *strtok_r(char *str, const char *delim, char **saveptr){
    char *p = str ? str : *saveptr;
    if (!p) return 0;
    while (*p && in_set_(*p, delim)) p++;
    if (!*p){ *saveptr = p; return 0; }
    char *start = p;
    while (*p && !in_set_(*p, delim)) p++;
    if (*p){ *p = 0; p++; }
    *saveptr = p;
    return start;
}
char *strtok(char *str, const char *delim){
    static char *save = 0;
    return strtok_r(str, delim, &save);
}

/* ---- stdlib stubs ---- */
void abort(void){ for(;;); }
void exit(int s){ (void)s; for(;;); }
int atexit(void (*f)(void)){ (void)f; return 0; }
/* ---- numeric conversion -------------------------------------------
 * The earlier versions of strtol/strtoul ignored `base`, which quietly
 * turns every hex string Mono parses (GUIDs, MONO_* env knobs, IL
 * offsets in logs) into 0.  These honour base 0/2..36 properly.
 */
static int digit_val_(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'z') return c - 'a' + 10;
    if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
    return -1;
}

static unsigned long long strtoull_core_(const char *s, char **e, int base, int *neg)
{
    const char *start = s;
    unsigned long long v = 0;
    int any = 0, d;

    *neg = 0;
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r' || *s == '\f' || *s == '\v')
        s++;
    if (*s == '+' || *s == '-') { *neg = (*s == '-'); s++; }

    if (base == 0) {
        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; s += 2; }
        else if (s[0] == '0') { base = 8; s++; any = 1; }
        else base = 10;
    } else if (base == 16 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
    }

    while ((d = digit_val_((unsigned char)*s)) >= 0 && d < base) {
        v = v * (unsigned long long)base + (unsigned long long)d;
        any = 1;
        s++;
    }
    if (e) *e = (char *)(any ? s : start);
    return v;
}

long strtol(const char *s, char **e, int b)
{
    int neg;
    unsigned long long v = strtoull_core_(s, e, b, &neg);
    return neg ? -(long)v : (long)v;
}

unsigned long strtoul(const char *s, char **e, int b)
{
    int neg;
    unsigned long long v = strtoull_core_(s, e, b, &neg);
    return neg ? (unsigned long)(-(long)v) : (unsigned long)v;
}

long long strtoll(const char *s, char **e, int b)
{
    int neg;
    unsigned long long v = strtoull_core_(s, e, b, &neg);
    return neg ? -(long long)v : (long long)v;
}

unsigned long long strtoull(const char *s, char **e, int b)
{
    int neg;
    unsigned long long v = strtoull_core_(s, e, b, &neg);
    return neg ? (unsigned long long)(-(long long)v) : v;
}

/* Decimal-only float parser: enough for the "1.5"-shaped values Mono reads
 * out of env knobs.  No hex floats, no inf/nan spellings. */
double strtod(const char *s, char **e)
{
    const char *start = s;
    double v = 0.0, frac = 0.1;
    int neg = 0, any = 0;

    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    if (*s == '+' || *s == '-') { neg = (*s == '-'); s++; }
    while (*s >= '0' && *s <= '9') { v = v * 10.0 + (*s - '0'); s++; any = 1; }
    if (*s == '.') {
        s++;
        while (*s >= '0' && *s <= '9') { v += (*s - '0') * frac; frac *= 0.1; s++; any = 1; }
    }
    if (any && (*s == 'e' || *s == 'E')) {
        const char *save = s;
        int eneg = 0, ev = 0, edig = 0;
        s++;
        if (*s == '+' || *s == '-') { eneg = (*s == '-'); s++; }
        while (*s >= '0' && *s <= '9') { ev = ev * 10 + (*s - '0'); s++; edig = 1; }
        if (!edig) s = save;
        else {
            double m = 1.0;
            while (ev-- > 0) m *= 10.0;
            v = eneg ? v / m : v * m;
        }
    }
    if (e) *e = (char *)(any ? s : start);
    return neg ? -v : v;
}

float strtof(const char *s, char **e) { return (float)strtod(s, e); }

int       atoi (const char *s){ return (int)strtol(s, 0, 10); }
long      atol (const char *s){ return strtol(s, 0, 10); }
long long atoll(const char *s){ return strtoll(s, 0, 10); }
double    atof (const char *s){ return strtod(s, 0); }

int       abs  (int j)      { return j < 0 ? -j : j; }
long      labs (long j)     { return j < 0 ? -j : j; }
long long llabs(long long j){ return j < 0 ? -j : j; }

/* C89 div/ldiv/lldiv.  C99 已经要求 / 和 % 都向零截断，正好就是
 * div() 的定义，所以直接用内建运算符即可。 */
div_t   div  (int n, int d)             { div_t   r; r.quot = n / d; r.rem = n % d; return r; }
ldiv_t  ldiv (long n, long d)           { ldiv_t  r; r.quot = n / d; r.rem = n % d; return r; }
lldiv_t lldiv(long long n, long long d) { lldiv_t r; r.quot = n / d; r.rem = n % d; return r; }

void *bsearch(const void *key, const void *base, size_t nmemb, size_t size,
              int (*cmp)(const void *, const void *))
{
    size_t lo = 0, hi = nmemb;
    const unsigned char *a = (const unsigned char *)base;

    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int r = cmp(key, a + mid * size);
        if (r == 0) return (void *)(a + mid * size);
        if (r < 0)  hi = mid;
        else        lo = mid + 1;
    }
    return 0;
}

char *getenv(const char *n){ (void)n; return 0; }
int setenv(const char *n, const char *v, int ow){ (void)n; (void)v; (void)ow; return 0; }
int unsetenv(const char *n){ (void)n; return 0; }
/* insertion sort: tiny, stable, no recursion depth worries in ring-3 */
void qsort(void *base, size_t nmemb, size_t size,
           int (*cmp)(const void *, const void *))
{
    unsigned char *a = (unsigned char *)base;
    for (size_t i = 1; i < nmemb; i++) {
        for (size_t j = i; j > 0 && cmp(a + (j - 1) * size, a + j * size) > 0; j--) {
            unsigned char *p = a + (j - 1) * size, *q = a + j * size;
            for (size_t k = 0; k < size; k++) { unsigned char t = p[k]; p[k] = q[k]; q[k] = t; }
        }
    }
}
int system(const char *c){ (void)c; return -1; }
static unsigned long g_rand_state = 123456789ul;
long random(void){ g_rand_state = g_rand_state*1103515245ul + 12345ul; return (long)(g_rand_state & 0x7fffffff); }
void srandom(unsigned int s){ g_rand_state = s ? s : 1ul; }
int  rand(void){ return (int)random(); }
void srand(unsigned int s){ srandom(s); }

/* ---- time stub ---- */
time_t time(time_t *t){ if(t)*t=0; return 0; }
int gettimeofday(struct timeval *tv, void *tz){ (void)tz; if(tv){tv->tv_sec=0;tv->tv_usec=0;} return 0; }

/* ---- 64-bit integer division helpers --------------------------------
 * On i386 the compiler lowers `long long` div/mod to libgcc calls.  We
 * are -nostdlib and must stay self-contained (the same object has to
 * link into MiniOS ring-3), so provide them here.  Shift-subtract only,
 * no 64-bit division inside.
 */
static unsigned long long udivmod64(unsigned long long n, unsigned long long d,
                                    unsigned long long *rem)
{
    unsigned long long q = 0, r = 0;
    if (d == 0) { if (rem) *rem = 0; return ~0ULL; }
    for (int i = 63; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1ULL);
        if (r >= d) { r -= d; q |= (1ULL << i); }
    }
    if (rem) *rem = r;
    return q;
}
unsigned long long __udivdi3(unsigned long long a, unsigned long long b){ return udivmod64(a, b, 0); }
unsigned long long __umoddi3(unsigned long long a, unsigned long long b){ unsigned long long r; udivmod64(a, b, &r); return r; }
unsigned long long __udivmoddi4(unsigned long long a, unsigned long long b, unsigned long long *r){ return udivmod64(a, b, r); }
long long __divdi3(long long a, long long b){
    int s = 0; unsigned long long ua, ub;
    if (a < 0) { s ^= 1; ua = (unsigned long long)(-a); } else ua = (unsigned long long)a;
    if (b < 0) { s ^= 1; ub = (unsigned long long)(-b); } else ub = (unsigned long long)b;
    unsigned long long q = udivmod64(ua, ub, 0);
    return s ? -(long long)q : (long long)q;
}
long long __moddi3(long long a, long long b){
    int s = (a < 0); unsigned long long ua, ub, r;
    ua = (a < 0) ? (unsigned long long)(-a) : (unsigned long long)a;
    ub = (b < 0) ? (unsigned long long)(-b) : (unsigned long long)b;
    udivmod64(ua, ub, &r);
    return s ? -(long long)r : (long long)r;
}

/* ---- libgcc bit-count / bit-scan helpers ----------------------------
 * gcc lowers __builtin_popcount / ctz / clz to these when it cannot use a
 * single instruction (no -mpopcnt on a generic i386 target).  We link
 * -nostdlib, so libgcc is not on the link line and we own them.
 */
int __popcountsi2(unsigned int a){
    a = a - ((a >> 1) & 0x55555555u);
    a = (a & 0x33333333u) + ((a >> 2) & 0x33333333u);
    a = (a + (a >> 4)) & 0x0F0F0F0Fu;
    return (int)((a * 0x01010101u) >> 24);
}
int __popcountdi2(unsigned long long a){
    return __popcountsi2((unsigned int)a) + __popcountsi2((unsigned int)(a >> 32));
}
int __ctzsi2(unsigned int a){
    int n = 0;
    if (!a) return 32;
    while (!(a & 1u)) { a >>= 1; n++; }
    return n;
}
int __clzsi2(unsigned int a){
    int n = 0;
    if (!a) return 32;
    while (!(a & 0x80000000u)) { a <<= 1; n++; }
    return n;
}
int __ctzdi2(unsigned long long a){
    unsigned int lo = (unsigned int)a;
    return lo ? __ctzsi2(lo) : 32 + __ctzsi2((unsigned int)(a >> 32));
}
int __clzdi2(unsigned long long a){
    unsigned int hi = (unsigned int)(a >> 32);
    return hi ? __clzsi2(hi) : 32 + __clzsi2((unsigned int)a);
}
int __ffsdi2(unsigned long long a){ return a ? __ctzdi2(a) + 1 : 0; }
int __paritysi2(unsigned int a){ return __popcountsi2(a) & 1; }

/* ---- minimal printf family ------------------------------------------
 * Supports  %[-+ 0#][width][.prec][hh|h|l|ll|z][diouxXcsp%]
 *
 * Two properties matter a lot to eglib:
 *   1. C99 measure mode.  g_vasprintf calls vsnprintf(NULL, 0, ...) first
 *      to size the buffer, so the return value MUST be the length the
 *      output *would* have had, independent of `size`.
 *   2. It must never write past size-1 and must always NUL-terminate
 *      when size > 0.
 */
typedef struct { char *buf; size_t size; size_t n; } fmtsink;

static void sink_putc(fmtsink *k, char c)
{
    if (k->buf && k->n + 1 < k->size) k->buf[k->n] = c;
    k->n++;                       /* always count: measure mode */
}
static void sink_pad(fmtsink *k, char c, int count)
{
    while (count-- > 0) sink_putc(k, c);
}

static int vsnprintf_core(char *buf, size_t size, const char *fmt, va_list ap)
{
    fmtsink k = { buf, size, 0 };
    const char *p = fmt;
    char tmp[32];

    while (*p) {
        if (*p != '%') { sink_putc(&k, *p++); continue; }
        p++;
        if (*p == '%') { sink_putc(&k, '%'); p++; continue; }

        /* flags */
        int left = 0, zero = 0, plus = 0, space = 0, alt = 0;
        for (;; p++) {
            if      (*p == '-') left  = 1;
            else if (*p == '0') zero  = 1;
            else if (*p == '+') plus  = 1;
            else if (*p == ' ') space = 1;
            else if (*p == '#') alt   = 1;
            else break;
        }
        /* width */
        int width = 0;
        if (*p == '*') { width = va_arg(ap, int); p++; if (width < 0) { left = 1; width = -width; } }
        else while (*p >= '0' && *p <= '9') width = width * 10 + (*p++ - '0');
        /* precision */
        int prec = -1;
        if (*p == '.') {
            p++; prec = 0;
            if (*p == '*') { prec = va_arg(ap, int); p++; }
            else while (*p >= '0' && *p <= '9') prec = prec * 10 + (*p++ - '0');
        }
        /* length modifier */
        int lng = 0;                       /* 0=int 1=long 2=long long */
        for (;;) {
            if (*p == 'l') { lng = (lng == 1) ? 2 : 1; p++; }
            else if (*p == 'h' || *p == 'z' || *p == 'j' || *p == 't') { p++; }
            else break;
        }

        char spec = *p ? *p++ : 0;
        const char *digits = "0123456789abcdef";
        int         ndig   = 0;
        char        sign   = 0;
        const char *prefix = "";

        switch (spec) {
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            int slen = 0;
            while (s[slen] && (prec < 0 || slen < prec)) slen++;
            if (!left) sink_pad(&k, ' ', width - slen);
            for (int i = 0; i < slen; i++) sink_putc(&k, s[i]);
            if (left) sink_pad(&k, ' ', width - slen);
            continue;
        }
        case 'c': {
            char c = (char)va_arg(ap, int);
            if (!left) sink_pad(&k, ' ', width - 1);
            sink_putc(&k, c);
            if (left) sink_pad(&k, ' ', width - 1);
            continue;
        }
        case 'd': case 'i': {
            long long v = (lng == 2) ? va_arg(ap, long long)
                        : (lng == 1) ? (long long)va_arg(ap, long)
                                     : (long long)va_arg(ap, int);
            unsigned long long uv;
            if (v < 0) { sign = '-'; uv = (unsigned long long)(-v); }
            else       { uv = (unsigned long long)v; if (plus) sign = '+'; else if (space) sign = ' '; }
            do { tmp[ndig++] = digits[uv % 10]; uv /= 10; } while (uv);
            break;
        }
        case 'u': case 'o': case 'x': case 'X': case 'p': {
            unsigned long long uv;
            unsigned base = (spec == 'u') ? 10u : (spec == 'o') ? 8u : 16u;
            if (spec == 'p') { uv = (unsigned long long)(unsigned long)va_arg(ap, void *); prefix = "0x"; }
            else uv = (lng == 2) ? va_arg(ap, unsigned long long)
                    : (lng == 1) ? (unsigned long long)va_arg(ap, unsigned long)
                                 : (unsigned long long)va_arg(ap, unsigned int);
            if (alt && base == 16 && uv) prefix = (spec == 'X') ? "0X" : "0x";
            do { tmp[ndig++] = digits[uv % base]; uv /= base; } while (uv);
            if (spec == 'X') for (int i = 0; i < ndig; i++)
                if (tmp[i] >= 'a') tmp[i] = (char)(tmp[i] - 'a' + 'A');
            break;
        }
        default:
            sink_putc(&k, '?');
            continue;
        }

        /* numeric emit: [sign][prefix][zeros][digits] with width/prec */
        {
            int plen = 0; while (prefix[plen]) plen++;
            int zeros = (prec > ndig) ? prec - ndig : 0;
            int total = ndig + zeros + plen + (sign ? 1 : 0);
            if (zero && !left && prec < 0 && width > total) { zeros += width - total; total = width; }
            if (!left) sink_pad(&k, ' ', width - total);
            if (sign) sink_putc(&k, sign);
            for (int i = 0; i < plen; i++) sink_putc(&k, prefix[i]);
            sink_pad(&k, '0', zeros);
            while (ndig > 0) sink_putc(&k, tmp[--ndig]);
            if (left) sink_pad(&k, ' ', width - total);
        }
    }

    if (buf && size) buf[(k.n < size) ? k.n : size - 1] = 0;
    return (int)k.n;
}
int vsnprintf(char *str, size_t size, const char *fmt, va_list ap){ return vsnprintf_core(str, size, fmt, ap); }
int snprintf(char *str, size_t size, const char *fmt, ...){
    va_list ap; va_start(ap, fmt); int r = vsnprintf_core(str, size, fmt, ap); va_end(ap); return r;
}
int sprintf(char *str, const char *fmt, ...){
    va_list ap; va_start(ap, fmt); int r = vsnprintf_core(str, (size_t)-1, fmt, ap); va_end(ap); return r;
}
int printf(const char *fmt, ...){
    char b[256]; va_list ap; va_start(ap, fmt);
    int r = vsnprintf_core(b, sizeof(b), fmt, ap); va_end(ap);
    if (pal_console_putc) for (int i=0;b[i];i++) pal_console_putc(b[i]);
    return r;
}
int fprintf(PAL_FILE *stream, const char *fmt, ...){
    (void)stream; char b[256]; va_list ap; va_start(ap, fmt);
    int r = vsnprintf_core(b, sizeof(b), fmt, ap); va_end(ap);
    if (pal_console_putc) for (int i=0;b[i];i++) pal_console_putc(b[i]);
    return r;
}
int fputs(const char *s, PAL_FILE *stream){ (void)stream; if (pal_console_putc) while(*s) pal_console_putc(*s++); return 0; }
int puts(const char *s){ fputs(s, 0); if (pal_console_putc) pal_console_putc('\n'); return 0; }
int fputc(int c, PAL_FILE *stream){ (void)stream; if (pal_console_putc) pal_console_putc((char)c); return c; }
int fflush(PAL_FILE *stream){ (void)stream; return 0; }

int vfprintf(PAL_FILE *stream, const char *fmt, va_list ap){
    char buf[512];
    int n = vsnprintf_core(buf, sizeof(buf), fmt, ap);
    (void)stream;
    if (pal_console_putc) for (int i=0;i<n && buf[i];i++) pal_console_putc(buf[i]);
    return n;
}
int vsprintf(char *str, const char *fmt, va_list ap){
    return vsnprintf_core(str, (size_t)-1, fmt, ap);
}
int vprintf(const char *fmt, va_list ap){ return vfprintf(0, fmt, ap); }

/* ---- math lives in pal/libm_impl.c (x87) ---- */

/* ---- unistd surface -------------------------------------------------
 * Phase 0: only fd 1/2 are real (they funnel to pal_console_putc).  A
 * harness may override by installing pal_write_hook / pal_read_hook,
 * which is how Phase 1 will route these to MiniOS SYS_WRITE / SYS_READ.
 */
int (*pal_write_hook)(int fd, const void *buf, unsigned len) = 0;
int (*pal_read_hook )(int fd, void *buf, unsigned len)       = 0;

ssize_t write(int fd, const void *buf, size_t count)
{
    if (pal_write_hook) return pal_write_hook(fd, buf, (unsigned)count);
    if ((fd == 1 || fd == 2) && pal_console_putc) {
        const char *p = (const char *)buf;
        for (size_t i = 0; i < count; i++) pal_console_putc(p[i]);
        return (ssize_t)count;
    }
    return -1;
}
ssize_t read(int fd, void *buf, size_t count)
{
    if (pal_read_hook) return pal_read_hook(fd, buf, (unsigned)count);
    (void)fd; (void)buf; (void)count;
    return 0;   /* EOF */
}
int   close (int fd){ (void)fd; return 0; }
off_t lseek (int fd, off_t off, int whence){ (void)fd; (void)off; (void)whence; return -1; }
int   unlink(const char *p){ (void)p; return -1; }
int   access(const char *p, int m){ (void)p; (void)m; return -1; }
char *getcwd(char *buf, size_t size){ if (buf && size > 1){ buf[0]='/'; buf[1]=0; return buf; } return 0; }
int   isatty(int fd){ return (fd == 1 || fd == 2) ? 1 : 0; }
pid_t getpid(void){ return 1; }
uid_t getuid(void){ return 0; }
unsigned int sleep(unsigned int s){ (void)s; return 0; }
int   usleep(unsigned int us){ (void)us; return 0; }
