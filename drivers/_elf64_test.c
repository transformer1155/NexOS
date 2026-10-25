/* Host harness: drives the REAL driver_elf.c ELF64 path on the real
 * fontvec.drv image, so validate_header()'s inputs can be inspected field by
 * field instead of guessed at from a serial log. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../drivers/driver_contract.h"

typedef struct ElfDriverImage ElfDriverImage;
int  driver_elf_read(const char* path, void** out_base, uint32_t* out_size,
                     ElfDriverImage** out_image);
void* elf_image_header(const ElfDriverImage* img);

/* ---- kernel services stubbed for the host ---- */
static unsigned char* g_file;
static int            g_file_len;

void* kmalloc(unsigned int n) { return malloc(n); }
void  kfree(void* p)          { free(p); }
void  serial_puts(const char* s){ fputs(s, stdout); }

int kern_fs_read(const char* name, unsigned char* buf, int bufsize)
{
    (void)name;
    if (!g_file) return -1;
    int n = g_file_len < bufsize ? g_file_len : bufsize;
    memcpy(buf, g_file, (size_t)n);
    return n;
}
int kern_fs_size(const char* name) { (void)name; return g_file ? g_file_len : -1; }

/* memcpy/memset come from libc in this hosted harness. */

int main(int argc, char** argv)
{
    const char* path = argc > 1 ? argv[1] : "build/fontvec.drv";

    FILE* f = fopen(path, "rb");
    if (!f) { printf("cannot open %s\n", path); return 2; }
    fseek(f, 0, SEEK_END);
    g_file_len = (int)ftell(f);
    fseek(f, 0, SEEK_SET);
    g_file = (unsigned char*)malloc((size_t)g_file_len);
    if (fread(g_file, 1, (size_t)g_file_len, f) != (size_t)g_file_len) return 2;
    fclose(f);
    printf("image: %s (%d bytes)\n\n", path, g_file_len);

    void* base = 0; uint32_t size = 0; ElfDriverImage* img = 0;
    int rc = driver_elf_read(path, &base, &size, &img);
    printf("\n=== driver_elf_read rc=%d ===\n", rc);
    if (rc != 0) return 1;

    DriverHeader* h = (DriverHeader*)elf_image_header(img);
    printf("header at %p\n", (void*)h);
    printf("  magic        = 0x%08X (want 0x%08X) %s\n", h->magic, DRV_MAGIC,
           h->magic == DRV_MAGIC ? "OK" : "BAD");
    printf("  abi_version  = 0x%08X (want 0x%08X) %s\n", h->abi_version, DRV_ABI_VERSION,
           h->abi_version == DRV_ABI_VERSION ? "OK" : "BAD");
    printf("  version      = 0x%08X\n", h->version);
    printf("  header_size  = %u (want %u) %s\n", h->header_size, (unsigned)sizeof(DriverHeader),
           (h->header_size >= sizeof(DriverHeader) && h->header_size <= size) ? "OK" : "BAD");
    printf("  name         = %p", (void*)h->name);
    if (h->name) printf("  \"%s\" len=%u (need 1..%u)", h->name, (unsigned)strlen(h->name), DRV_MAX_NAME - 1);
    printf("\n");
    printf("  init         = %p %s\n", (void*)h->init, h->init ? "OK" : "NULL");
    printf("  unload       = %p %s\n", (void*)h->unload, h->unload ? "OK" : "NULL");
    printf("  symbol_count = %u (max %u) %s\n", h->symbol_count, DRV_MAX_SYMBOLS,
           h->symbol_count <= DRV_MAX_SYMBOLS ? "OK" : "BAD");
    printf("  symbols      = %p %s\n", (void*)h->symbols,
           (h->symbol_count && !h->symbols) ? "NULL" : "OK");
    if (h->symbols) {
        for (uint32_t i = 0; i < h->symbol_count; i++)
            printf("    [%u] name=%p \"%s\" addr=%p\n", i, (void*)h->symbols[i].name,
                   h->symbols[i].name ? h->symbols[i].name : "(null)",
                   h->symbols[i].addr);
    }

    /* replicate validate_header()'s decision */
    int ok = 1;
    if (h->magic != DRV_MAGIC) ok = 0;
    if (h->abi_version != DRV_ABI_VERSION) ok = 0;
    if (h->header_size < sizeof(DriverHeader) || h->header_size > size) ok = 0;
    if (!h->name || strlen(h->name) == 0 || strlen(h->name) >= DRV_MAX_NAME) ok = 0;
    if (!h->init || !h->unload) ok = 0;
    if (h->symbol_count > DRV_MAX_SYMBOLS) ok = 0;
    if (h->symbol_count && !h->symbols) ok = 0;
    printf("\nvalidate_header => %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
