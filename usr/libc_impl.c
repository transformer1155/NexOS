/* =====================================================================
 *  usr/libc_impl.c  -  NexOS native user-runtime implementation (P1)
 * ---------------------------------------------------------------------
 *  Freestanding libc for .nex executables (no _start; that lives in
 *  libc.c).  All syscalls go through
 *  int 0x80 (Linux i386 ABI).  Heap is a first-fit free list carved out
 *  of a static pool (no brk needed).  No external dependencies.
 * ===================================================================== */
#include "libc.h"
#include <stdarg.h>

/* ----------------------------------------------------------------- */
/*  Syscall wrapper                                                   */
/* ----------------------------------------------------------------- */
static inline long do_syscall(long num, long a, long b, long c)
{
    long ret;
    __asm__ volatile ("int $0x80"
        : "=a"(ret)
        : "a"(num), "b"(a), "c"(b), "d"(c)
        : "memory", "cc");
    return ret;
}

long nex_write(int fd, const void* buf, unsigned long count)
{
    return do_syscall(4, (long)fd, (long)buf, (long)count);
}

void nex_exit(int code)
{
    do_syscall(1, (long)code, 0, 0);
    for (;;) { }            /* never reached; keeps noreturn semantics */
}

/* ----------------------------------------------------------------- */
/*  Linux socket bridge (guest TCP to host)                          */
/* ----------------------------------------------------------------- */
long nex_socket(void)
{
    return do_syscall(400, 0, 0, 0);
}

long nex_connect(unsigned long ip, unsigned long port)
{
    return do_syscall(401, (long)ip, (long)port, 0);
}

long nex_send(const void* buf, int len)
{
    return do_syscall(402, (long)buf, (long)len, 0);
}

long nex_recv(void* buf, int len)
{
    return do_syscall(403, (long)buf, (long)len, 0);
}

void nex_sock_close(void)
{
    do_syscall(404, 0, 0, 0);
}

/* ----------------------------------------------------------------- */
/*  Cross-.so export test (Stage 6)                                   */
/* ----------------------------------------------------------------- */
int nex_add(int a, int b)
{
    return a + b;
}

/* ----------------------------------------------------------------- */
/*  Heap: first-fit free list over a static pool                      */
/* ----------------------------------------------------------------- */
#define NEX_HEAP_SIZE (256 * 1024)
#define NEX_BLK_HDR   (sizeof(struct nex_blk))

typedef struct nex_blk {
    size_t            size;     /* usable bytes (excludes header) */
    int               free;
    struct nex_blk*   next;
} nex_blk_t;

static unsigned char nex_heap[NEX_HEAP_SIZE] __attribute__((aligned(8)));
static nex_blk_t*     nex_heap_head = 0;

static void nex_heap_init(void)
{
    if (nex_heap_head) return;
    nex_heap_head = (nex_blk_t*)nex_heap;
    nex_heap_head->size = NEX_HEAP_SIZE - NEX_BLK_HDR;
    nex_heap_head->free = 1;
    nex_heap_head->next = 0;
}

void* malloc(size_t n)
{
    nex_heap_init();
    if (n == 0) n = 1;
    n = (n + 7) & ~(size_t)7;            /* 8-byte align */

    nex_blk_t* p = nex_heap_head;
    while (p) {
        if (p->free && p->size >= n) {
            /* Split if the remainder is large enough for another block. */
            if (p->size >= n + NEX_BLK_HDR + 16) {
                nex_blk_t* nb = (nex_blk_t*)((unsigned char*)p + NEX_BLK_HDR + n);
                nb->size = p->size - n - NEX_BLK_HDR;
                nb->free = 1;
                nb->next = p->next;
                p->size = n;
                p->next = nb;
            }
            p->free = 0;
            return (unsigned char*)p + NEX_BLK_HDR;
        }
        p = p->next;
    }
    return 0;
}

void free(void* ptr)
{
    if (!ptr) return;
    nex_blk_t* p = (nex_blk_t*)((unsigned char*)ptr - NEX_BLK_HDR);
    p->free = 1;
    /* Coalesce adjacent free blocks. */
    nex_blk_t* cur = nex_heap_head;
    while (cur && cur->next) {
        if (cur->free && cur->next->free) {
            cur->size += NEX_BLK_HDR + cur->next->size;
            cur->next = cur->next->next;
        } else {
            cur = cur->next;
        }
    }
}

void* calloc(size_t n, size_t sz)
{
    size_t t = n * sz;
    void* p = malloc(t);
    if (p) memset(p, 0, t);
    return p;
}

void* realloc(void* ptr, size_t n)
{
    if (!ptr) return malloc(n);
    if (n == 0) { free(ptr); return 0; }
    nex_blk_t* b = (nex_blk_t*)((unsigned char*)ptr - NEX_BLK_HDR);
    size_t old = b->size;
    void* np = malloc(n);
    if (!np) return 0;
    size_t c = (n < old) ? n : old;
    memcpy(np, ptr, c);
    free(ptr);
    return np;
}

/* ----------------------------------------------------------------- */
/*  String / memory primitives                                        */
/* ----------------------------------------------------------------- */
size_t strlen(const char* s)
{
    const char* p = s;
    while (*p) p++;
    return (size_t)(p - s);
}

int strcmp(const char* a, const char* b)
{
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char* a, const char* b, size_t n)
{
    while (n-- && *a && *a == *b) { a++; b++; }
    return n == (size_t)-1 ? 0 : (unsigned char)*a - (unsigned char)*b;
}

char* strcpy(char* d, const char* s)
{
    char* r = d;
    while ((*d++ = *s++));
    return r;
}

char* strcat(char* d, const char* s)
{
    char* r = d;
    while (*d) d++;
    while ((*d++ = *s++));
    return r;
}

char* strncpy(char* d, const char* s, size_t n)
{
    char* r = d;
    while (n-- && (*d = *s)) { d++; s++; }
    return r;
}

void* memcpy(void* d, const void* s, size_t n)
{
    unsigned char* dp = (unsigned char*)d;
    const unsigned char* sp = (const unsigned char*)s;
    while (n--) *dp++ = *sp++;
    return d;
}

void* memmove(void* d, const void* s, size_t n)
{
    unsigned char* dp = (unsigned char*)d;
    const unsigned char* sp = (const unsigned char*)s;
    if (dp < sp) { while (n--) *dp++ = *sp++; }
    else { dp += n; sp += n; while (n--) *--dp = *--sp; }
    return d;
}

void* memset(void* d, int v, size_t n)
{
    unsigned char* dp = (unsigned char*)d;
    unsigned char c = (unsigned char)v;
    while (n--) *dp++ = c;
    return d;
}

int memcmp(const void* a, const void* b, size_t n)
{
    const unsigned char* ap = (const unsigned char*)a;
    const unsigned char* bp = (const unsigned char*)b;
    while (n--) {
        if (*ap != *bp) return (int)*ap - (int)*bp;
        ap++; bp++;
    }
    return 0;
}

int atoi(const char* s)
{
    int r = 0;
    while (*s >= '0' && *s <= '9') { r = r * 10 + (*s - '0'); s++; }
    return r;
}

/* ----------------------------------------------------------------- */
/*  stdio                                                              */
/* ----------------------------------------------------------------- */
int putchar(int c)
{
    char ch = (char)c;
    return (int)nex_write(1, &ch, 1);
}

int puts(const char* s)
{
    nex_write(1, s, strlen(s));
    putchar('\n');
    return 0;
}

/* Minimal, dependency-free formatter.  Supports:
 *   %s %c %d %u %x %p %%   (and field width ignored)                 */
static void nex_utoa(char** p, unsigned long v, int base, int upper)
{
    char tmp[16];
    int i = 0;
    const char* dig = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    if (v == 0) tmp[i++] = '0';
    while (v) { tmp[i++] = dig[v % base]; v /= base; }
    while (i > 0) { **p = tmp[--i]; (*p)++; }
}

int vprintf(const char* fmt, va_list ap)
{
    char buf[256];
    char* p = buf;
    const char* f = fmt;
    while (*f) {
        if (*f != '%') { *p++ = *f++; continue; }
        f++;
        if (*f == '%') { *p++ = '%'; f++; continue; }
        if (*f == 's') {
            const char* s = va_arg(ap, const char*);
            if (!s) s = "(null)";
            while (*s) *p++ = *s++;
            f++; continue;
        }
        if (*f == 'c') {
            *p++ = (char)va_arg(ap, int);
            f++; continue;
        }
        if (*f == 'd' || *f == 'i') {
            int v = va_arg(ap, int);
            if (v < 0) { *p++ = '-'; nex_utoa(&p, (unsigned long)(-v), 10, 0); }
            else nex_utoa(&p, (unsigned long)v, 10, 0);
            f++; continue;
        }
        if (*f == 'u') {
            nex_utoa(&p, (unsigned long)va_arg(ap, unsigned int), 10, 0);
            f++; continue;
        }
        if (*f == 'x' || *f == 'X') {
            nex_utoa(&p, (unsigned long)va_arg(ap, unsigned int), 16, *f == 'X');
            f++; continue;
        }
        if (*f == 'p') {
            *p++ = '0'; *p++ = 'x';
            nex_utoa(&p, (unsigned long)va_arg(ap, void*), 16, 0);
            f++; continue;
        }
        /* Unknown specifier: emit verbatim. */
        *p++ = '%'; *p++ = *f++;
    }
    return (int)nex_write(1, buf, (unsigned long)(p - buf));
}

int printf(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vprintf(fmt, ap);
    va_end(ap);
    return r;
}

/* ----------------------------------------------------------------- */
