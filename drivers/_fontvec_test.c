/* Host harness: proves the driver wrapper's contract mechanics without a
 * kernel.  Exercises init/unload, the reserved[] API handshake and the
 * device registration -- exactly what driver_load() would drive. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "../drivers/driver_contract.h"
#include "../font_vec.h"

/* font_vec imports, satisfied for the host by libc/bump allocator. */
#include <stdlib.h>
extern "C" void* kmalloc(unsigned int n) { return malloc(n); }
extern "C" void  kfree(void* p)           { free(p); }

typedef struct FontVecApi_mirror {
    int  (*init)(int (*rf)(int, const char*, uint8_t*, int));
    int  (*ready)(void);
    const uint8_t* (*glyph)(uint32_t, int, int*, int*, int*, int*);
    int  (*advance)(uint32_t, int);
    void (*set_dpr)(int);
    void (*last_err)(int*, int*, int*);
} FontVecApi_mirror;

/* The driver image is built as C, so these have C linkage. */
extern "C" {
    extern DriverHeader nexos_driver_header;
    extern const FontVecApi_mirror* nexos_fontvec_api(void);
}

static int g_dev_registered;
static void* g_dev_ops;
static char g_log[4096];
static int  g_log_n;

static void* t_kmalloc(uint32_t n) { return malloc(n); }
static void t_kfree(void* p)       { free(p); }
static void t_printk(const char* s) {
    int i = 0; while (s[i] && g_log_n < (int)sizeof(g_log) - 1) g_log[g_log_n++] = s[i++];
    g_log[g_log_n] = 0;
}
static int t_reg(const char* name, void* ops) {
    if (strcmp(name, "fontvec") == 0) { g_dev_registered = 1; g_dev_ops = ops; return 0; }
    return -1;
}
static int t_unreg(const char* name) { (void)name; g_dev_registered = 0; return 0; }

static int fails;
static void chk(int cond, const char* what) {
    printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) fails++;
}

int main(void) {
    printf("=== fontvec driver wrapper contract test ===\n");

    DriverHeader* h = &nexos_driver_header;
    chk(h->magic == DRV_MAGIC,                    "header magic == DRV_MAGIC");
    chk(h->abi_version == DRV_ABI_VERSION,        "abi_version == DRV_ABI_VERSION");
    chk(h->header_size == sizeof(DriverHeader),   "header_size matches ABI");
    chk(strcmp(h->name, "fontvec") == 0,          "name == \"fontvec\"");
    chk(h->symbol_count == 4,                     "declares exactly 4 imports");
    chk(h->symbols && strcmp(h->symbols[0].name,"kmalloc")==0
        && strcmp(h->symbols[1].name,"kfree")==0
        && strcmp(h->symbols[2].name,"memcpy")==0
        && strcmp(h->symbols[3].name,"memset")==0, "imports are kmalloc/kfree/memcpy/memset");
    chk(h->init && h->unload,                      "init + unload present");

    DriverKernelAPI api;
    memset(&api, 0, sizeof(api));
    api.abi_version       = DRV_ABI_VERSION;
    api.kmalloc           = t_kmalloc;
    api.kfree             = t_kfree;
    api.printk            = t_printk;
    api.register_device   = t_reg;
    api.unregister_device = t_unreg;

    /* pre-condition: no API published yet */
    chk(h->reserved[0] == 0 && h->reserved[1] == 0, "API slot empty before init");

    int rc = h->init(&api, (void*)0x1234);
    chk(rc == DRIVER_OK,                           "init() returns DRIVER_OK");
    chk(g_dev_registered && g_dev_ops != 0,        "register_device(\"fontvec\") called with ops");

    /* recover the table from reserved[] the way the kernel shim will */
    uintptr_t v = (uintptr_t)h->reserved[0] | ((uintptr_t)h->reserved[1] << 32);
    const FontVecApi_mirror* from_slot = (const FontVecApi_mirror*)v;
    chk(from_slot != 0,                            "reserved[] decodes to a pointer");
    chk(from_slot == nexos_fontvec_api(),          "reserved[] == the published API table");
    chk(from_slot == (const FontVecApi_mirror*)g_dev_ops, "reserved[] == register_device ops");

    chk(from_slot->init && from_slot->ready && from_slot->glyph
        && from_slot->advance && from_slot->set_dpr && from_slot->last_err,
        "all 6 API entries populated");

    /* vec_ready() must be honest: the font is NOT loaded until vec_init runs,
     * which is what keeps the GUI on the bitmap fallback. */
    chk(from_slot->ready() == 0,                   "ready()==0 before vec_init (bitmap fallback intact)");
    chk(from_slot->advance('A', 16) == 16,         "advance() falls back to px when not inited");
    {
        int gw = 0, gh = 0, gx = 0, gy = 0;
        chk(from_slot->glyph('A', 16, &gw, &gh, &gx, &gy) == 0,
            "glyph() returns NULL when not inited");
    }

    /* hot reload: a second init must be idempotent */
    rc = h->init(&api, (void*)0x1234);
    chk(rc == DRIVER_OK,                           "second init() is idempotent");

    /* unload withdraws the API so the kernel shim degrades gracefully */
    h->unload((void*)0x1234);
    chk(h->reserved[0] == 0 && h->reserved[1] == 0, "unload() withdraws the API slot");
    chk(!g_dev_registered,                         "unload() unregisters the device");

    printf("--- driver log ---\n%s", g_log);
    printf("\n=== %s (%d failure%s) ===\n", fails ? "FAILED" : "ALL PASS",
           fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
