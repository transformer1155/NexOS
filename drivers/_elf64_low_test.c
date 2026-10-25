/* Host harness v2: same as _elf64_test but the allocator hands out LOW
 * addresses (~0x10000000), matching what kernel64's kmalloc returns.  The
 * earlier harness used brk/malloc (~0x7d7d...), which would hide any
 * 32/32S truncation or bounds bug that only shows up for low bases. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "../drivers/driver_contract.h"

typedef struct ElfDriverImage ElfDriverImage;
int  driver_elf_read(const char* path, void** out_base, uint32_t* out_size,
                     ElfDriverImage** out_image);
void* elf_image_header(const ElfDriverImage* img);

static unsigned char* g_file;
static int            g_file_len;

/* ---- bump allocator over a LOW mapped region ---- */
#define LOW_BASE ((void*)0x10000000UL)
#define LOW_SIZE (64UL * 1024 * 1024)
static unsigned char* g_arena;
static size_t         g_used;

static void arena_init(void)
{
    void* p = mmap(LOW_BASE, LOW_SIZE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (p == MAP_FAILED) {
        p = mmap(NULL, LOW_SIZE, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        printf("WARN: could not map LOW_BASE; using %p instead\n", p);
    }
    g_arena = (unsigned char*)p;
    g_used = 0;
}

void* kmalloc(unsigned int n)
{
    if (!g_arena) arena_init();
    size_t a = (g_used + 15u) & ~(size_t)15u;
    if (a + n > LOW_SIZE) return NULL;
    g_used = a + n;
    return g_arena + a;
}
void  kfree(void* p) { (void)p; }   /* arena: never reused, like a kernel leak */
void  serial_puts(const char* s) { fputs(s, stdout); }

int kern_fs_read(const char* name, unsigned char* buf, int bufsize)
{
    (void)name;
    if (!g_file) return -1;
    int n = g_file_len < bufsize ? g_file_len : bufsize;
    memcpy(buf, g_file, (size_t)n);
    return n;
}
int kern_fs_size(const char* name) { (void)name; return g_file ? g_file_len : -1; }

int main(int argc, char** argv)
{
    const char* path = argc > 1 ? argv[1] : "build/fontvec.drv";
    FILE* f = fopen(path, "rb");
    if (!f) { printf("cannot open %s\n", path); return 2; }
    fseek(f, 0, SEEK_END); g_file_len = (int)ftell(f); fseek(f, 0, SEEK_SET);
    g_file = (unsigned char*)malloc((size_t)g_file_len);
    if (fread(g_file, 1, (size_t)g_file_len, f) != (size_t)g_file_len) return 2;
    fclose(f);

    void* base = 0; uint32_t size = 0; ElfDriverImage* img = 0;
    int rc = driver_elf_read(path, &base, &size, &img);
    printf("driver_elf_read rc=%d\n", rc);
    if (rc != 0) { printf("RESULT: FAIL (loader refused image)\n"); return 1; }

    DriverHeader* h = (DriverHeader*)elf_image_header(img);
    printf("header=%p magic=0x%08X name=%s syms=%u init=%p unload=%p\n",
           (void*)h, h->magic, h->name ? h->name : "(null)",
           h->symbol_count, (void*)h->init, (void*)h->unload);

    int ok = (h->magic == DRV_MAGIC) && (h->abi_version == DRV_ABI_VERSION) &&
             h->name && strlen(h->name) && h->init && h->unload &&
             h->symbol_count <= DRV_MAX_SYMBOLS &&
             (h->symbol_count == 0 || h->symbols);
    printf("RESULT: %s\n", ok ? "PASS (image loaded + header valid)" : "FAIL (header invalid)");
    return ok ? 0 : 1;
}
