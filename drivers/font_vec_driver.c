/* ============================================================================
 * font_vec_driver.c - font_vec hot-loadable driver wrapper (NexOS)
 * ----------------------------------------------------------------------------
 * Purpose: architecture decoupling, not space saving.  The vector-font
 * rasteriser (font_vec.c + stb_truetype) is product/plugin code, not kernel
 * core, so it belongs in a hot-loadable .drv image rather than being linked
 * into the kernel binary.
 *
 * This file is the WRAPPER: font_vec.c is compiled into the same .drv image
 * (see the Makefile rule) and this file publishes the contract descriptor
 * plus the function table the kernel needs to reach the rasteriser.
 *
 * -------------------------------------------------------------------------
 * HOW THE KERNEL FINDS THE RASTERISER
 * -------------------------------------------------------------------------
 * DriverHeader carries only init/unload/suspend/resume, so a driver that
 * exports an API needs a side channel.  This driver uses two, deliberately:
 *
 *   1. register_device("fontvec", &g_fontvec_api)
 *      The contract-sanctioned route: the loader records the ops pointer in
 *      its own per-driver device table.
 *
 *   2. DriverHeader.reserved[0..1] holds the same table pointer, so the
 *      kernel can recover it from driver_find("fontvec") alone with no
 *      framework change.  The slot count depends on pointer width, hence
 *      the ptr_put/ptr_get helpers below (32-bit uses reserved[0] only,
 *      a 64-bit image uses reserved[0..1]).
 *
 *      reserved[] exists precisely for out-of-band extensions like this and
 *      the layout never changes, so the ABI stays 1.0.
 *
 * -------------------------------------------------------------------------
 * GRACEFUL DEGRADATION (hard requirement)
 * -------------------------------------------------------------------------
 * gui.cpp guards every vector-font call site with `if (g_font_mode != 0 &&
 * vec_ready())` and otherwise draws the baked bitmap font.  This driver must
 * therefore NEVER make vec_ready() lie and must never fault the kernel when
 * it is absent:
 *   - if the image is not loaded, the kernel-side shim reports not-ready and
 *     the bitmap path runs;
 *   - if the TTF cannot be read, vec_init() returns <0 (harmless) and the
 *     bitmap path runs;
 *   - a failed load is logged by the loader and skipped -- no panic.
 *
 * -------------------------------------------------------------------------
 * BUILD
 * -------------------------------------------------------------------------
 * freestanding, ET_REL, no libc.  See the Makefile rule
 * `$(SFS_DIR)/fontvec.drv`.  The image links font_vec.c + this file with
 * `ld -r`; the four kernel imports (kmalloc/kfree/memcpy/memset) are
 * resolved by the loader, not by static linking.
 * ==========================================================================*/
#include "driver_contract.h"
#include "font_vec.h"          /* -I.. from drivers/ : the real vec_* API  */

/* ---------------------------------------------------------------------------
 * The rasteriser entry points, linked into this same image from font_vec.c.
 * Declaring them through font_vec.h (above) means the table below is
 * type-checked against the real signatures: if font_vec.h changes shape,
 * this file stops compiling instead of silently mismatching.
 * -------------------------------------------------------------------------*/
typedef struct FontVecApi {
    /* mirrors font_vec.h exactly ------------------------------------------ */
    int  (*init)(int (*read_file)(int fs_type, const char* name,
                                  uint8_t* buf, int bufsize));
    int  (*ready)(void);
    const uint8_t* (*glyph)(uint32_t cp, int px, int* w, int* h,
                            int* xoff, int* yoff);
    int  (*advance)(uint32_t cp, int px);
    void (*set_dpr)(int factor);
    void (*last_err)(int* code, int* got, int* stb);
} FontVecApi;

/* Filled at compile time: every entry is a link-time constant inside the
 * image, so there is nothing to patch on load and no init-order hazard. */
static const FontVecApi g_fontvec_api = {
    vec_init,
    vec_ready,
    vec_glyph,
    vec_advance,
    vec_set_dpr,
    vec_last_err,
};

/* ---------------------------------------------------------------------------
 * Pointer smuggling into DriverHeader.reserved[]
 *   reserved is uint32_t[4].  On a 32-bit build a pointer fits in slot 0; on
 *   a 64-bit build it needs slots 0..1.  Both helpers compile away to
 *   nothing surprising in either width.
 * -------------------------------------------------------------------------*/
#define FONTVEC_RES_SLOT 0

static void ptr_put(uint32_t res[4], const void* p)
{
    uintptr_t v = (uintptr_t)p;
    res[FONTVEC_RES_SLOT] = (uint32_t)(v & 0xFFFFFFFFu);
#if __SIZEOF_POINTER__ == 8
    res[FONTVEC_RES_SLOT + 1] = (uint32_t)(v >> 32);
#else
    res[FONTVEC_RES_SLOT + 1] = 0;
#endif
}

/* The mirror-image reader lives on the kernel side (it is the consumer of
 * these slots, so keeping it there avoids an unused static here). */

/* ---------------------------------------------------------------------------
 * Lifecycle
 * -------------------------------------------------------------------------*/
/* Defined at the bottom of this file; referenced by init/unload to publish
 * and withdraw the API pointer in reserved[]. */
extern DriverHeader nexos_driver_header;

static const DriverKernelAPI* g_api;
static int g_live;

static int fontvec_init(const DriverKernelAPI* api, void* ctx)
{
    /* ctx belongs to the loader's registry record; the driver owns its own
     * state and must not reinterpret it. */
    (void)ctx;

    if (!api) return DRIVER_ERR_INVALID_ARG;
    if (api->abi_version != DRV_ABI_VERSION) return DRIVER_ERR_ABI_MISMATCH;

    g_api  = api;

    /* Publish the entry points BOTH ways (see the file header).  This happens
     * before any printk so that even a console failure leaves the API
     * reachable. */
    ptr_put(nexos_driver_header.reserved, &g_fontvec_api);

    if (api->register_device) {
        int rc = api->register_device("fontvec", (void*)&g_fontvec_api);
        if (rc != DRIVER_OK) {
            /* Not fatal: reserved[] already carries the table, so the kernel
             * can still use the rasteriser.  Log and continue. */
            if (api->printk) api->printk("[FONTVEC] register_device failed (reserved[] still valid)\n");
        }
    }

    g_live = 1;

    if (api->printk) {
        api->printk("[FONTVEC] init: rasteriser published, ABI ");
        api->printk("1.0");
        api->printk(", vec_init awaits the kernel read_file callback\n");
    }
    return DRIVER_OK;
}

static void fontvec_unload(void* ctx)
{
    const DriverKernelAPI* api = g_api;
    (void)ctx;

    if (!g_live) return;      /* never initialised, or already unloaded */
    g_live = 0;

    /* Stop advertising the API FIRST: from here on the kernel-side shim sees
     * "not loaded" and the GUI falls back to the bitmap font, which is the
     * safe state to be in while teardown runs. */
    ptr_put(nexos_driver_header.reserved, 0);

    if (api) {
        if (api->unregister_device) api->unregister_device("fontvec");
        if (api->printk) api->printk("[FONTVEC] unload: API withdrawn, GUI falls back to bitmap font\n");
    }

    /* NOTE: font_vec.c keeps its TTF buffer (g_ttf_buf) and parsed font info
     * for the lifetime of the process; it exposes no teardown entry point.
     * So a hot unload returns the code+API but not that one allocation until
     * reboot.  Documented rather than papered over: adding vec_shutdown()
     * means changing font_vec.c, which is outside this task's write scope. */
    g_api = 0;
}

/* ---------------------------------------------------------------------------
 * Descriptor
 * -------------------------------------------------------------------------*/
/* Exactly what font_vec.c imports (verified with `nm -u font_vec.o`):
 * kmalloc/kfree for the stb scratch + TTF buffers, memcpy/memset which the
 * compiler emits for struct copies and initialisers.  Declaring them here
 * makes a kernel that does not export one fail the load loudly instead of
 * faulting inside the rasteriser. */
static DriverSymbolReq g_fontvec_symbols[] = {
    { "kmalloc", 0 },
    { "kfree",   0 },
    { "memcpy",  0 },
    { "memset",  0 },
    { 0, 0 }
};

static const char* const g_fontvec_deps[] = { 0 };   /* no dependencies */

/* Non-const: the loader fills symbols[].addr in place. */
DriverHeader nexos_driver_header = {
    DRV_MAGIC,              /* magic                                  */
    DRV_ABI_VERSION,        /* abi_version                            */
    0x00010000u,            /* version 1.0                            */
    sizeof(DriverHeader),   /* header_size                            */
    "fontvec",              /* name (driver_find target)              */
    "TrueType vector font rasteriser (font_vec.c + stb_truetype)",
    g_fontvec_deps,         /* deps                                   */
    g_fontvec_symbols,      /* symbols                                */
    4,                      /* symbol_count                          */
    fontvec_init,           /* init                                   */
    fontvec_unload,         /* unload                                 */
    0,                      /* suspend (optional)                     */
    0,                      /* resume  (optional)                     */
    0,                      /* flags                                  */
    { 0, 0, 0, 0 }          /* reserved[0..1] carry the API pointer   */
};

/* Exported so the kernel-side shim can read the table back without parsing
 * the reserved[] slots itself (used by the 32-bit path and by host tests).
 * The loader picks the first symbol beginning with "nexos_driver_header",
 * so this is the descriptor above -- keep that ordering guarantee. */
const FontVecApi* nexos_fontvec_api(void)
{
    return &g_fontvec_api;
}
