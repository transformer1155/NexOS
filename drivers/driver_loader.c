/* ============================================================================
 * driver_loader.c - NexOS hot-loadable driver loader (kernel side)
 * ----------------------------------------------------------------------------
 * Responsibilities
 *   1. image ingest  -> validate magic / ABI before trusting one byte of it
 *   2. symbol binding-> resolve the driver's imports against kernel exports
 *   3. lifecycle     -> init / unload with strict ordering + rollback
 *   4. resource fence-> per-driver refcount so unload can never race a call
 *
 * Design notes that matter for safety
 *   - The driver image is NOT trusted.  Magic, header_size, abi_version and
 *     every string length are validated before any dereference.
 *   - Resources allocated by a driver at init() time are recorded in the
 *     registry (devices, irqs) so unload can force-release them even if the
 *     driver's own unload() forgot.  This is the anti-panic guarantee: a
 *     buggy driver must not be able to wedge the kernel by leaking an IRQ.
 *   - hot reload is: pin old -> load new (staged, init not yet run) ->
 *     wait for old refcount to drain -> unload old -> commit new.  If any
 *     step fails we restore the previously live driver, never a half state.
 *
 * Freestanding: no libc.  Uses the kernel's kmalloc/kfree and serial console.
 * ==========================================================================*/
#include "driver_contract.h"

/* ---- kernel services this file itself needs ------------------------------ */
extern void* kmalloc(unsigned int size);
extern void  kfree(void* ptr);
extern void  serial_puts(const char* s);
extern void  serial_putc(char c);
extern void  serial_puts_dec(int v);
extern void  serial_hex(uint32_t v);
extern int   kern_fs_read(const char* name, unsigned char* buf, int bufsize);
extern int   kern_fs_size(const char* name);

/* ---- real ELF/SFS driver image backend (driver_elf.c) ------------------- */
typedef struct ElfDriverImage ElfDriverImage;
int  driver_elf_read(const char* path, void** out_base, uint32_t* out_size,
                     ElfDriverImage** out_image);
/* Address of the DriverHeader export inside a loaded image (NULL if none). */
void* elf_image_header(const ElfDriverImage* img);
void driver_elf_free(ElfDriverImage* img);
/* Non-zero once the SFS filesystem is mounted and kern_fs_read() is usable. */
extern int  kern_fs_ready(void);
/* ---- boot-time driver discovery ----------------------------------------
 * The kernel owns the filesystem layout, so it also owns directory
 * enumeration: nexos_fs_drv_count()/nexos_fs_drv_name() list the .drv images
 * present on the mounted SFS.  They are declared weak so the host-side test
 * harness (which has no SFS) still links. */
extern int  nexos_fs_drv_count(void) __attribute__((weak));
extern const char* nexos_fs_drv_name(int idx) __attribute__((weak));
/* Physical page allocator + CPUID, used by hardware-virtualization drivers. */
extern uint32_t pmm_alloc_page(void);
extern void     pmm_free_page(uint32_t pa);
extern uint32_t pmm_alloc_range(uint32_t pages);
extern void     pmm_free_range(uint32_t pa, uint32_t pages);
extern void     nexos_cpuid(uint32_t leaf, uint32_t sub,
                            uint32_t* a, uint32_t* b, uint32_t* c, uint32_t* d);

/* ---------------------------------------------------------------------------
 * Module-1 stand-in for the real image reader.  Replacing this with an ELF
 * loader later touches exactly one function.  It returns a heap buffer that
 * the caller owns (freed via kfree) or NULL on failure.
 * -------------------------------------------------------------------------*/
typedef struct DrvImage {
    DriverHeader* header;
    void*         base;
    uint32_t      size;
} DrvImage;

/* Mock: hand-rolled "images" for the framework bring-up tests.  Order of
 * declaration does not matter; the registry picks them up by name. */
extern const DriverHeader sample_driver_v1;   /* from sample_driver.c      */
extern const DriverHeader sample_driver_v2;   /* version-bump variant      */

/* Each live driver owns its loaded image; teardown frees it. */
/* (handle is stored on DriverRec::image) */

/* ---------------------------------------------------------------------------
 * image_read(): the real ingest path.
 *
 *   SFS "/drivers/<name>.drv"  ->  driver_elf_read()  ->  validate -> load
 *
 * If SFS is not mounted yet (early boot / host test harness) we fall back to
 * the built-in sample descriptors so the lifecycle logic stays testable.
 * That fallback is compiled out on a kernel build via DRV_NO_MOCK_FALLBACK.
 * -------------------------------------------------------------------------*/
/* ---------------------------------------------------------------------------
 * ELF-ident check with retry.
 *
 * WHY: the ATA/SFS path occasionally hands back a stale or torn buffer.  That is
 * not theoretical -- a boot was observed where the 32-bit driver scan reported
 * "ELF load failed" (bad magic) for ALL THREE .drv images, while the 64-bit
 * scan of the same SFS on the same disk, in the same boot, succeeded.  The
 * 64-bit kernel's staging read already guards against this with re-reads plus a
 * magic check (task-14); the driver scan had no such guard, so a single bad read
 * silently produced a boot with ZERO drivers loaded.
 *
 * The retry covers only the 4-byte ident read.  That is deliberate: it is the
 * cheap, leak-free place to detect the torn buffer, and it catches exactly the
 * failure that was observed.  Parsing (driver_elf_read) is not retried, because
 * a failed parse may already have taken the image buffer apart; retrying it
 * could compound a leak.  A parse that still fails after a good ident is a real
 * image problem and gets reported as such.
 * -------------------------------------------------------------------------*/
#define DRV_READ_ATTEMPTS 4

static int drv_ident_readable(const char* path)
{
    unsigned char id[4];
    int attempt;
    for (attempt = 0; attempt < DRV_READ_ATTEMPTS; attempt++) {
        int n = kern_fs_read(path, id, (int)sizeof(id));
        if (n >= 4 && id[0] == 0x7F && id[1] == 'E' && id[2] == 'L' && id[3] == 'F')
            return 1;
        /* Short pause before re-reading: let an outstanding controller
         * transfer retire instead of racing it again immediately. */
        {
            volatile int spin;
            for (spin = 0; spin < 50000; spin++) { }
        }
    }
    return 0;
}
static int image_read(const char* path, void** out_base, uint32_t* out_size,
                      ElfDriverImage** out_img)
{
    if (!path || !out_base || !out_size) return DRIVER_ERR_INVALID_ARG;
    if (out_img) *out_img = 0;

    /* --- preferred: real ELF from SFS -------------------------------- */
#ifndef DRV_NO_MOCK_FALLBACK
    int have_fs = kern_fs_ready();
#else
    int have_fs = 1;
#endif
    if (have_fs) {
        /* Verify the ident (with retries) BEFORE parsing: a stale buffer here
         * is what turns into a driver-less boot. */
        if (!drv_ident_readable(path)) {
            serial_puts("[DRIVER] image unreadable (bad ELF ident after ");
            serial_putc((char)('0' + DRV_READ_ATTEMPTS));
            serial_puts(" attempts): ");
            serial_puts(path);
            serial_puts("\n");
            return DRIVER_ERR_IO;
        }

        uint32_t size = 0;
        void*    base = 0;
        ElfDriverImage* img = 0;
        int rc = driver_elf_read(path, &base, &size, &img);
        if (rc == DRIVER_OK) {
            /* The loader treats out_base as the DriverHeader.  For ELF the
             * descriptor is an exported symbol inside the image, NOT the
             * image start, so publish img->header here. */
            *out_base = elf_image_header(img);
            *out_size = size;
            if (out_img) *out_img = img;
            serial_puts("[DRIVER] ELF image loaded: ");
            serial_puts(path);
            serial_puts("\n");
            return DRIVER_OK;
        }
        /* A real filesystem that cannot satisfy the request is a hard error:
         * silently substituting a mock driver would mask a packaging bug. */
        serial_puts("[DRIVER] ELF load failed rc=");
        serial_putc((char)('0' + ((rc < 0 ? -rc : rc) % 10)));
        serial_puts("\n");
        return rc;
    }

    /* No filesystem yet: nothing can be loaded.  The old built-in mock
     * descriptors were removed when the real ELF/SFS backend landed -- a
     * driver that silently degraded to a mock would hide a packaging bug. */
    serial_puts("[DRIVER] no filesystem mounted, cannot load image\n");
    return DRIVER_ERR_IO;
}

/* ---- small freestanding helpers ----------------------------------------- */
static int str_eq(const char* a, const char* b)
{
    if (!a || !b) return 0;
    while (*a && *b) { if (*a != *b) return 0; ++a; ++b; }
    return *a == *b;
}

static int str_len(const char* s)
{
    int n = 0;
    if (!s) return 0;
    while (s[n]) ++n;
    return n;
}

/* Copy at most cap-1 bytes and always NUL-terminate. Returns length. */
static int str_copy(char* dst, const char* src, int cap)
{
    int i = 0;
    if (!dst || cap <= 0) return 0;
    if (src) { while (src[i] && i < cap - 1) { dst[i] = src[i]; ++i; } }
    dst[i] = 0;
    return i;
}

static void puts_hex(uint32_t v)
{
    static const char* h = "0123456789ABCDEF";
    char buf[11];
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 0; i < 8; ++i) buf[2 + i] = h[(v >> (28 - i * 4)) & 0xF];
    buf[10] = 0;
    serial_puts(buf);
}

static void puts_dec(uint32_t v)
{
    char b[12]; int n = 0;
    if (v == 0) { serial_puts("0"); return; }
    while (v && n < 11) { b[n++] = (char)('0' + (v % 10u)); v /= 10u; }
    char o[12]; int k = 0;
    while (n) o[k++] = b[--n];
    o[k] = 0;
    serial_puts(o);
}

/* ---- kernel export table (symbol resolution) ----------------------------
 * Module 7 requirement 2: drivers must bind to kernel APIs by name.  We
 * publish a tiny table the loader searches; an unresolved import is a
 * hard load failure, never a NULL call.                                     */
typedef struct KernelExport { const char* name; void* addr; } KernelExport;

/* Driver-visible kernel symbols.  Keep this list minimal and explicit:
 * every entry is a permanent ABI promise to out-of-tree .drv images.
 *
 * memcpy/memset are required by almost every freestanding C module -- the
 * compiler emits calls to them for struct copies and initialisers, so a
 * driver that omits them fails to relocate rather than merely misbehaving.
 * font_vec.c additionally needs kmalloc/kfree for its stb_truetype scratch
 * buffers. */
extern void* memcpy(void* d, const void* s, unsigned int n);
extern void* memset(void* d, int v, unsigned int n);

static const KernelExport g_exports[] = {
    { "kmalloc",           (void*)&kmalloc          },
    { "kfree",             (void*)&kfree            },
    { "printk",            (void*)&serial_puts      },
    { "memcpy",            (void*)&memcpy           },
    { "memset",            (void*)&memset           },
    { "kern_fs_read",      (void*)&kern_fs_read     },
    { "kern_fs_ready",     (void*)&kern_fs_ready    },
    { "kern_fs_size",      (void*)&kern_fs_size     },
    { "serial_puts_dec",  (void*)&serial_puts_dec },
    { "serial_hex",        (void*)&serial_hex       },
};
static const int g_export_count = (int)(sizeof(g_exports) / sizeof(g_exports[0]));

static void* resolve_symbol(const char* name)
{
    if (!name) return 0;
    for (int i = 0; i < g_export_count; ++i)
        if (str_eq(g_exports[i].name, name)) return g_exports[i].addr;
    return 0;
}

/* ---- spinlock ------------------------------------------------------------
 * NexOS has no generic spinlock yet, so the framework carries its own.
 * Lock order is always  g_lock  ->  drv->lock  (never the reverse).          */
typedef struct Spinlock { volatile uint32_t locked; } Spinlock;

static void lock_acquire(Spinlock* l)
{
    if (!l) return;
    while (__sync_lock_test_and_set(&l->locked, 1u)) {
        __asm__ __volatile__("pause");
    }
    __sync_synchronize();
}

static void lock_release(Spinlock* l)
{
    if (!l) return;
    __sync_synchronize();
    __sync_lock_release(&l->locked);
}

static Spinlock g_lock;      /* guards g_drivers[] + g_count              */

/* ---- per-driver registry record ---------------------------------------- */
typedef struct DriverDeviceRec {
    char  name[DRV_MAX_NAME];
    void* ops;
    int   used;
} DriverDeviceRec;

typedef struct DriverIrqRec {
    uint8_t irq;
    int     used;
} DriverIrqRec;

typedef struct DriverRec {
    const DriverHeader* hdr;
    char                name[DRV_MAX_NAME];
    DriverState         state;
    DriverStatus        last_error;

    /* unload fence: non-zero while somebody is executing driver code */
    volatile int        refcount;
    volatile int        unload_pending;

    char                path[DRV_MAX_PATH];
    uint32_t            image_size;

    /* resources we force-release if the driver's own unload() forgets them */
    DriverDeviceRec     devices[DRV_MAX_DEVICES];
    DriverIrqRec        irqs[4];
    uint32_t            alloc_count;

    DriverAddrSpace     as;     /* DRV_ISO_NONE today */

    /* Owned ELF image (driver_elf.c).  Freed in teardown(); NULL when the
     * driver came from the built-in mock fallback. */
    ElfDriverImage*     image;
} DriverRec;

static DriverRec    g_drivers[DRV_MAX_DRIVERS];
static int          g_count;
static DriverStats  g_stats;
static DriverKernelAPI g_api;

/* ---- forward decls of the kernel-side hooks we hand to drivers ---------- */
static DriverRec* current_driver_for_calls(void);

static void* api_kmalloc(uint32_t n)                    { return kmalloc(n); }
static void  api_kfree(void* p)                         { kfree(p); }
static void  api_printk(const char* s)                  { serial_puts(s); }

static int api_register_device(const char* name, void* ops)
{
    if (!name) return DRIVER_ERR_INVALID_ARG;
    DriverRec* d = current_driver_for_calls();
    if (!d) return DRIVER_ERR_INVALID_ARG;
    /* init() runs while the record is DRV_STATE_LOADED -- that is exactly
     * when a driver is supposed to register its devices.  Hot reload runs
     * init() against an already-INITED record.  Accept both, reject the
     * teardown/fault states. */
    if (d->state != DRV_STATE_LOADED && d->state != DRV_STATE_INITED)
        return DRIVER_ERR_INIT_FAILED;

    for (int i = 0; i < DRV_MAX_DEVICES; ++i) {
        if (!d->devices[i].used) {
            str_copy(d->devices[i].name, name, DRV_MAX_NAME);
            d->devices[i].ops  = ops;
            d->devices[i].used = 1;
            serial_puts("[DRIVER] register_device(");
            serial_puts(name);
            serial_puts(")\n");
            return DRIVER_OK;
        }
    }
    return DRIVER_ERR_NO_SPACE;
}

static int api_unregister_device(const char* name)
{
    if (!name) return DRIVER_ERR_INVALID_ARG;
    DriverRec* d = current_driver_for_calls();
    if (!d) return DRIVER_ERR_INVALID_ARG;
    for (int i = 0; i < DRV_MAX_DEVICES; ++i) {
        if (d->devices[i].used && str_eq(d->devices[i].name, name)) {
            d->devices[i].used = 0;
            d->devices[i].ops  = 0;
            return DRIVER_OK;
        }
    }
    return DRIVER_ERR_NOT_FOUND;
}

static int  api_request_irq(uint8_t irq, void (*handler)(void*), void* ctx);
static void api_free_irq(uint8_t irq);
static uint32_t api_pmm_alloc_page(void)              { return pmm_alloc_page(); }
static void     api_pmm_free_page(uint32_t pa)        { pmm_free_page(pa); }
static uint32_t api_pmm_alloc_range(uint32_t pages)   { return pmm_alloc_range(pages); }
static void     api_pmm_free_range(uint32_t pa, uint32_t n) { pmm_free_range(pa, n); }
static void     api_cpuid(uint32_t leaf, uint32_t sub,
                          uint32_t* a, uint32_t* b, uint32_t* c, uint32_t* d)
{ nexos_cpuid(leaf, sub, a, b, c, d); }

static int  api_ref_get(void* ctx);
static void api_ref_put(void* ctx);
static int  api_unload_pending(void* ctx);

/* Which driver is currently executing?  Single-threaded bring-up keeps this
 * simple; with per-CPU data it becomes a GS-relative lookup. */
static DriverRec* g_current;

static DriverRec* current_driver_for_calls(void) { return g_current; }

int driver_subsystem_init(void)
{
    /* The boot path reaches this twice: once from the kernel's driver block
     * (after the heap is up, so images can actually be allocated) and again
     * from plugin_manager_boot().  Re-zeroing a populated registry on the
     * second call would orphan every image, device and IRQ record the first
     * call installed, so treat it as an idempotent "make sure the API vtable
     * is published" call instead. */
    if (g_count > 0) return DRIVER_OK;

    g_count = 0;
    g_current = 0;
    g_lock.locked = 0;
    for (int i = 0; i < DRV_MAX_DRIVERS; ++i) {
        g_drivers[i].hdr = 0;
        g_drivers[i].state = DRV_STATE_EMPTY;
        g_drivers[i].refcount = 0;
        g_drivers[i].unload_pending = 0;
        g_drivers[i].last_error = DRIVER_OK;
        g_drivers[i].path[0] = 0;
        g_drivers[i].image_size = 0;
        g_drivers[i].alloc_count = 0;
        for (int k = 0; k < DRV_MAX_DEVICES; ++k) g_drivers[i].devices[k].used = 0;
        for (int k = 0; k < 4; ++k) { g_drivers[i].irqs[k].used = 0; g_drivers[i].irqs[k].irq = 0; }
        g_drivers[i].as.kind = DRV_ISO_NONE;
        g_drivers[i].as.base = 0;
        g_drivers[i].as.size = 0;
        g_drivers[i].as.page_dir = 0;
        g_drivers[i].as.map = 0; g_drivers[i].as.unmap = 0;
        g_drivers[i].as.grant = 0; g_drivers[i].as.destroy = 0;
    }
    for (uint32_t i = 0; i < sizeof(g_stats); ++i) ((unsigned char*)&g_stats)[i] = 0;

    /* Publish the service vtable handed to every driver. */
    g_api.abi_version       = DRV_ABI_VERSION;
    g_api.kmalloc           = api_kmalloc;
    g_api.kfree             = api_kfree;
    g_api.printk            = api_printk;
    g_api.register_device   = api_register_device;
    g_api.unregister_device = api_unregister_device;
    g_api.request_irq       = api_request_irq;
    g_api.free_irq          = api_free_irq;
    g_api.pmm_alloc_page    = api_pmm_alloc_page;
    g_api.pmm_free_page     = api_pmm_free_page;
    g_api.pmm_alloc_range   = api_pmm_alloc_range;
    g_api.pmm_free_range    = api_pmm_free_range;
    g_api.cpuid             = api_cpuid;
    g_api.ref_get           = api_ref_get;
    g_api.ref_put           = api_ref_put;
    g_api.unload_pending    = api_unload_pending;

    serial_puts("[DRIVER] subsystem init, ABI ");
    puts_hex(DRV_ABI_VERSION);
    serial_puts("\n");
    return DRIVER_OK;
}

/* ---- registry helpers --------------------------------------------------- */
static DriverRec* find_rec(const char* name)
{
    if (!name) return 0;
    for (int i = 0; i < DRV_MAX_DRIVERS; ++i) {
        if (g_drivers[i].state != DRV_STATE_EMPTY &&
            str_eq(g_drivers[i].name, name)) return &g_drivers[i];
    }
    return 0;
}

static DriverRec* alloc_rec(void)
{
    for (int i = 0; i < DRV_MAX_DRIVERS; ++i)
        if (g_drivers[i].state == DRV_STATE_EMPTY) return &g_drivers[i];
    return 0;
}

/* ---- header validation --------------------------------------------------
 * Every check happens before any member other than magic is read, so a
 * malformed image can never steer execution.                                */
static DriverStatus validate_header(const DriverHeader* h, uint32_t img_size)
{
    if (!h) return DRIVER_ERR_INVALID_ARG;

    if (h->magic != DRV_MAGIC) {
        g_stats.rejected_magic++;
        return DRIVER_ERR_INVALID_MAGIC;
    }
    serial_puts("[DRIVER] Magic OK\n");

    if (h->abi_version != DRV_ABI_VERSION) {
        serial_puts("[DRIVER] ABI mismatch: got ");
        puts_hex(h->abi_version);
        serial_puts(" want ");
        puts_hex(DRV_ABI_VERSION);
        serial_puts("\n");
        return DRIVER_ERR_ABI_MISMATCH;
    }
    serial_puts("[DRIVER] ABI OK\n");

    if (h->header_size < sizeof(DriverHeader) || h->header_size > img_size)
        return DRIVER_ERR_BAD_VERSION;

    if (!h->name || str_len(h->name) == 0 || str_len(h->name) >= DRV_MAX_NAME)
        return DRIVER_ERR_INVALID_ARG;
    if (!h->init || !h->unload)
        return DRIVER_ERR_INVALID_ARG;   /* both hooks are mandatory */
    if (h->symbol_count > DRV_MAX_SYMBOLS)
        return DRIVER_ERR_INVALID_ARG;
    if (h->symbol_count && !h->symbols)
        return DRIVER_ERR_INVALID_ARG;

    serial_puts("[DRIVER] header valid: ");
    serial_puts(h->name);
    serial_puts(" v");
    puts_hex(h->version);
    serial_puts("\n");
    return DRIVER_OK;
}

/* ---- symbol binding ----------------------------------------------------- */
static DriverStatus bind_symbols(DriverHeader* h)
{
    if (!h) return DRIVER_ERR_INVALID_ARG;
    for (uint32_t i = 0; i < h->symbol_count; ++i) {
        DriverSymbolReq* r = &h->symbols[i];
        if (!r->name) return DRIVER_ERR_INVALID_ARG;
        void* a = resolve_symbol(r->name);
        if (!a) {
            serial_puts("[DRIVER] symbol NOT FOUND: ");
            serial_puts(r->name);
            serial_puts("\n");
            g_stats.rejected_symbols++;
            return DRIVER_ERR_SYMBOL_NOT_FOUND;
        }
        r->addr = a;
        serial_puts("[DRIVER] symbol ");
        serial_puts(r->name);
        serial_puts(" -> ");
        puts_hex((uint32_t)(uintptr_t)a);
        serial_puts("\n");
    }
    return DRIVER_OK;
}

/* ---- dependency check --------------------------------------------------- */
static DriverStatus check_deps(const DriverHeader* h)
{
    if (!h || !h->deps) return DRIVER_OK;
    for (int i = 0; i < DRV_MAX_DEPS && h->deps[i]; ++i) {
        if (!find_rec(h->deps[i])) {
            serial_puts("[DRIVER] dep not loaded: ");
            serial_puts(h->deps[i]);
            serial_puts("\n");
            return DRIVER_ERR_DEP_UNSATISFIED;
        }
    }
    return DRIVER_OK;
}

/* ---- address-space / isolation seam ------------------------------------
 * Today: DRV_ISO_NONE, everything runs in the kernel address space, so the
 * hooks stay null and callers check for them.  This is the reserved seam
 * required by the brief.                                                    */
static DriverStatus setup_isolation(DriverRec* d, const DriverHeader* h)
{
    if (!d || !h) return DRIVER_ERR_INVALID_ARG;
    d->as.kind   = DRV_ISO_NONE;
    d->as.base   = 0;
    d->as.size   = 0;
    d->as.map    = 0;
    d->as.unmap  = 0;
    d->as.grant  = 0;
    d->as.destroy= 0;

    if (h->flags & DRV_FLAG_RING3) {
        /* Demanded isolation we cannot provide yet: refuse loudly rather
         * than silently running privileged code under a false promise. */
        serial_puts("[DRIVER] isolation UNAVAILABLE (needs paging/ALC backend)\n");
        return DRIVER_ERR_ISOLATION;
    }
    serial_puts("[DRIVER] isolation: none (single kernel AS)\n");
    return DRIVER_OK;
}

/* ---- forced resource reclamation (anti-panic) -------------------------- */
static void reclaim_resources(DriverRec* d)
{
    if (!d) return;
    int reclaimed = 0;

    for (int i = 0; i < DRV_MAX_DEVICES; ++i) {
        if (d->devices[i].used) {
            serial_puts("[DRIVER] reclaim device: ");
            serial_puts(d->devices[i].name);
            serial_puts("\n");
            d->devices[i].used = 0;
            d->devices[i].ops  = 0;
            reclaimed++;
        }
    }
    for (int i = 0; i < 4; ++i) {
        if (d->irqs[i].used) {
            serial_puts("[DRIVER] reclaim IRQ ");
            puts_dec(d->irqs[i].irq);
            serial_puts("\n");
            if (g_api.free_irq) g_api.free_irq(d->irqs[i].irq);
            d->irqs[i].used = 0;
            reclaimed++;
        }
    }
    if (reclaimed) {
        g_stats.leaks_detected++;
        serial_puts("[DRIVER] WARNING: driver leaked ");
        puts_dec((uint32_t)reclaimed);
        serial_puts(" resource(s); reclaimed by kernel\n");
    }
}

/* ---- refcount fence ----------------------------------------------------- */
static int  api_ref_get(void* ctx)
{
    DriverRec* d = (DriverRec*)ctx;
    if (!d) return 0;
    if (d->unload_pending) return 0;     /* refuse new users during teardown */
    __sync_fetch_and_add(&d->refcount, 1);
    return 1;
}

static void api_ref_put(void* ctx)
{
    DriverRec* d = (DriverRec*)ctx;
    if (!d) return;
    if (__sync_fetch_and_sub(&d->refcount, 1) < 0) {
        /* Underflow: a driver released a reference it never took. Clamp and
         * record; leaving it negative would wedge every future unload. */
        d->refcount = 0;
        serial_puts("[DRIVER] BUG: ref_put underflow (clamped)\n");
    }
}

static int api_unload_pending(void* ctx)
{
    DriverRec* d = (DriverRec*)ctx;
    return d ? d->unload_pending : 1;
}

/* ---- IRQ registration ---------------------------------------------------
 * Interrupt table programming is deliberately out of scope for the skeleton;
 * we only record ownership so it can be reclaimed.                          */
static int api_request_irq(uint8_t irq, void (*handler)(void*), void* ctx)
{
    (void)handler; (void)ctx;
    DriverRec* d = current_driver_for_calls();
    if (!d) return DRIVER_ERR_INVALID_ARG;
    for (int i = 0; i < 4; ++i) {
        if (!d->irqs[i].used) {
            d->irqs[i].used = 1;
            d->irqs[i].irq  = irq;
            serial_puts("[DRIVER] request_irq(");
            puts_dec(irq);
            serial_puts(") recorded\n");
            return DRIVER_OK;
        }
    }
    return DRIVER_ERR_NO_SPACE;
}

static void api_free_irq(uint8_t irq)
{
    DriverRec* d = current_driver_for_calls();
    if (!d) return;
    for (int i = 0; i < 4; ++i)
        if (d->irqs[i].used && d->irqs[i].irq == irq) { d->irqs[i].used = 0; return; }
}

/* ---- teardown core ------------------------------------------------------
 * Order matters: fence new calls, run the driver's own unload(), reclaim
 * anything it forgot, then clear the registry slot so the name is free for
 * an immediate reload.                                                      */
static DriverStatus teardown(DriverRec* d)
{
    if (!d) return DRIVER_ERR_INVALID_ARG;

    serial_puts("[DRIVER] unloading ");
    serial_puts(d->name);
    serial_puts("\n");

    d->unload_pending = 1;
    d->state = DRV_STATE_UNLOADING;

    /* Wait for in-flight users to drain.  Bounded so a buggy driver cannot
     * hang the whole kernel forever; after the timeout we reclaim anyway
     * (that is strictly safer than a deadlock, and we log loudly). */
    uint32_t spins = 0;
    while (d->refcount > 0 && spins < 2000000u) { ++spins; __asm__ __volatile__("pause"); }
    if (d->refcount > 0) {
        serial_puts("[DRIVER] WARN: refcount stuck at ");
        puts_dec((uint32_t)d->refcount);
        serial_puts(", forcing unload\n");
    }

    DriverRec* prev = g_current;
    g_current = d;
    if (d->hdr && d->hdr->unload) d->hdr->unload(d);
    g_current = prev;

    reclaim_resources(d);

    /* Release the loaded ELF image last: the driver's code lives in it, so
     * nothing may run from here on. */
    if (d->image) { driver_elf_free(d->image); d->image = 0; }

    d->refcount = 0;
    d->unload_pending = 0;
    d->hdr = 0;
    d->state = DRV_STATE_EMPTY;
    d->name[0] = 0;
    d->path[0] = 0;
    d->image_size = 0;
    d->alloc_count = 0;

    g_stats.unloads++;
    serial_puts("[DRIVER] unload complete\n");
    return DRIVER_OK;
}

/* ---- staged load --------------------------------------------------------
 * Loads and inits a driver, but does not yet make it visible under the
 * registry name.  Used by hot reload so the new image can fail safely.      */
static DriverStatus load_staged(const char* path, DriverRec** out)
{
    if (!path || !out) return DRIVER_ERR_INVALID_ARG;

    void* base = 0; uint32_t size = 0;
    ElfDriverImage* img = 0;
    DriverStatus st = image_read(path, &base, &size, &img);
    /* NOTE: img is retained on success; on any failure below it must be freed. */
    if (st != DRIVER_OK) {
        serial_puts("[DRIVER] cannot read image: ");
        serial_puts(path);
        serial_puts("\n");
        return st;
    }

    DriverHeader* h = (DriverHeader*)base;
    st = validate_header(h, size);
    if (st != DRIVER_OK) return st;

    DriverRec* d = alloc_rec();
    if (!d) return DRIVER_ERR_NO_SPACE;

    if (find_rec(h->name)) {
        serial_puts("[DRIVER] duplicate name: ");
        serial_puts(h->name);
        serial_puts("\n");
        return DRIVER_ERR_DUPLICATE;
    }

    st = check_deps(h);
    if (st != DRIVER_OK) return st;

    st = setup_isolation(d, h);
    if (st != DRIVER_OK) return st;

    st = bind_symbols(h);
    if (st != DRIVER_OK) return st;

    d->hdr        = h;
    str_copy(d->name, h->name, DRV_MAX_NAME);
    str_copy(d->path, path, DRV_MAX_PATH);
    d->image_size = size;
    d->refcount   = 1;              /* the caller's reference */
    d->unload_pending = 0;
    d->state      = DRV_STATE_LOADED;
    d->last_error = DRIVER_OK;

    serial_puts("[DRIVER] calling init() for ");
    serial_puts(d->name);
    serial_puts("\n");

    DriverRec* prev = g_current;
    g_current = d;
    int rc = h->init ? h->init(&g_api, d) : DRIVER_ERR_INIT_FAILED;
    g_current = prev;

    if (rc < 0) {
        serial_puts("[DRIVER] init FAILED rc=");
        puts_dec((uint32_t)(-rc));
        serial_puts("\n");
        reclaim_resources(d);       /* partial init may have allocated */
        if (d->image) { driver_elf_free(d->image); d->image = 0; }
        d->hdr = 0;
        d->state = DRV_STATE_FAULTED;
        d->last_error = DRIVER_ERR_INIT_FAILED;
        d->refcount = 0;
        return DRIVER_ERR_INIT_FAILED;
    }

    d->refcount = 0;                /* caller drops it once visible */
    d->state = DRV_STATE_INITED;
    g_stats.loads++;
    serial_puts("[DRIVER] Init Success\n");

    *out = d;
    return DRIVER_OK;
}

/* ---- public: load ------------------------------------------------------- */
int driver_load(const char* path)
{
    if (!path) return DRIVER_ERR_INVALID_ARG;

    lock_acquire(&g_lock);
    DriverRec* d = 0;
    DriverStatus st = load_staged(path, &d);
    if (st == DRIVER_OK) g_count++;
    lock_release(&g_lock);

    if (st != DRIVER_OK) return (int)st;
    serial_puts("[DRIVER] loaded: ");
    serial_puts(d->name);
    serial_puts("\n");
    return DRIVER_OK;
}

/* ---- public: unload ----------------------------------------------------- */
int driver_unload(const char* name)
{
    if (!name) return DRIVER_ERR_INVALID_ARG;

    lock_acquire(&g_lock);
    DriverRec* d = find_rec(name);
    if (!d || d->state == DRV_STATE_EMPTY) {
        lock_release(&g_lock);
        return DRIVER_ERR_NOT_FOUND;
    }
    if (d->hdr && (d->hdr->flags & DRV_FLAG_NO_HOT_RELOAD)) {
        serial_puts("[DRIVER] driver forbids unload\n");
        lock_release(&g_lock);
        return DRIVER_ERR_BUSY;
    }
    if (d->refcount > 0) {
        serial_puts("[DRIVER] busy, refcount=");
        puts_dec((uint32_t)d->refcount);
        serial_puts("\n");
        lock_release(&g_lock);
        return DRIVER_ERR_BUSY;
    }
    DriverStatus st = teardown(d);
    if (st == DRIVER_OK && g_count > 0) g_count--;
    lock_release(&g_lock);
    return (int)st;
}

/* ---- public: hot reload -------------------------------------------------
 * Atomic from the caller's point of view: on failure the old driver stays
 * live and untouched.  On success the name now refers to the new version.   */
int driver_hot_reload(const char* name, const char* new_path)
{
    if (!name || !new_path) return DRIVER_ERR_INVALID_ARG;

    lock_acquire(&g_lock);

    DriverRec* old = find_rec(name);
    if (!old || old->state != DRV_STATE_INITED) {
        lock_release(&g_lock);
        return DRIVER_ERR_NOT_FOUND;
    }
    if (old->hdr && (old->hdr->flags & DRV_FLAG_NO_HOT_RELOAD)) {
        lock_release(&g_lock);
        return DRIVER_ERR_BUSY;
    }

    /* Stage the replacement while the old one is still serving users. */
    void* base = 0; uint32_t size = 0;
    ElfDriverImage* new_img = 0;
    DriverStatus st = image_read(new_path, &base, &size, &new_img);
    if (st != DRIVER_OK) {
        g_stats.rollbacks++;
        lock_release(&g_lock);
        return (int)st;
    }
    DriverHeader* nh = (DriverHeader*)base;
    st = validate_header(nh, size);
    if (st != DRIVER_OK) {
        g_stats.rollbacks++;
        lock_release(&g_lock);
        return (int)st;
    }
    if (!str_eq(nh->name, name)) {
        /* Version bump must keep the same name, otherwise it is a new driver
         * and "reload" would silently change what the name means. */
        serial_puts("[DRIVER] hot reload name mismatch\n");
        g_stats.rollbacks++;
        lock_release(&g_lock);
        return DRIVER_ERR_INVALID_ARG;
    }
    if (nh->version <= old->hdr->version) {
        serial_puts("[DRIVER] reload requires a version bump\n");
        g_stats.rollbacks++;
        lock_release(&g_lock);
        return DRIVER_ERR_BAD_VERSION;
    }

    serial_puts("[DRIVER] hot reload ");
    serial_puts(name);
    serial_puts(" v");
    puts_hex(old->hdr->version);
    serial_puts(" -> v");
    puts_hex(nh->version);
    serial_puts("\n");

    /* Fence the old image: no new references from here on, drain the rest. */
    old->unload_pending = 1;
    uint32_t spins = 0;
    while (old->refcount > 0 && spins < 4000000u) { ++spins; __asm__ __volatile__("pause"); }

    if (old->refcount > 0) {
        serial_puts("[DRIVER] WARN: drain timed out, deferring reload\n");
        old->unload_pending = 0;
        g_stats.rollbacks++;
        lock_release(&g_lock);
        return DRIVER_ERR_BUSY;
    }

    /* Take the new driver live BEFORE tearing the old one down, so a failed
     * init leaves us with the old driver still registered and functional. */
    st = check_deps(nh);
    if (st != DRIVER_OK) {
        old->unload_pending = 0;
        g_stats.rollbacks++;
        lock_release(&g_lock);
        return (int)st;
    }
    st = bind_symbols(nh);
    if (st != DRIVER_OK) {
        old->unload_pending = 0;
        g_stats.rollbacks++;
        lock_release(&g_lock);
        return (int)st;
    }

    /* The new image owns the record's booking tables from here on.  Clear
     * the retiring image's entries first so a legit reload is not reported
     * as a double registration / phantom leak. */
    for (int i = 0; i < DRV_MAX_DEVICES; ++i) { old->devices[i].used = 0; old->devices[i].ops = 0; }
    for (int i = 0; i < 4; ++i) old->irqs[i].used = 0;

    DriverRec* prev = g_current;
    g_current = old;
    int rc = nh->init ? nh->init(&g_api, old) : DRIVER_ERR_INIT_FAILED;
    g_current = prev;

    if (rc < 0) {
        serial_puts("[DRIVER] new init failed, rolling back\n");
        reclaim_resources(old);
        if (new_img) { driver_elf_free(new_img); new_img = 0; }
        old->unload_pending = 0;
        g_stats.rollbacks++;
        lock_release(&g_lock);
        return DRIVER_ERR_ROLLBACK_FAILED;
    }

    /* New image is live. Now retire the old one's resources and rebind. */
    old->unload_pending = 0;
    old->state = DRV_STATE_UNLOADING;
    {
        DriverRec* save = g_current;
        g_current = old;
        if (old->hdr && old->hdr->unload) old->hdr->unload(old);
        g_current = save;
    }
    reclaim_resources(old);

    old->hdr        = nh;
    old->image_size = size;
    str_copy(old->path, new_path, DRV_MAX_PATH);
    old->state      = DRV_STATE_INITED;
    old->refcount   = 0;
    old->unload_pending = 0;
    old->last_error = DRIVER_OK;

    g_stats.hot_reloads++;
    serial_puts("[DRIVER] hot reload complete\n");
    lock_release(&g_lock);
    return DRIVER_OK;
}

/* ---- ELF-class filter ----------------------------------------------------
 * An image is only usable by the kernel whose pointer width it was built for:
 * DriverHeader and every pointer inside it are pointer-sized, so a 32-bit
 * kernel decoding an ELF64 header reads the fields at the wrong offsets and
 * fails validation with a confusing "load failed rc=3".  Filtering here,
 * before any parse or relocation pass, keeps the boot log honest.  Cost is
 * one 8-byte read per image.
 */
static int image_is_other_class(const char* path)
{
    unsigned char id[8];
    int n = kern_fs_read(path, id, (int)sizeof(id));
    if (n < 8) return 0;                      /* unreadable: let the loader report */
    if (id[0] != 0x7F || id[1] != 'E' || id[2] != 'L' || id[3] != 'F') return 0;
#if __SIZEOF_POINTER__ == 8
    return id[4] == 1;                        /* ELFCLASS32 image on a 64-bit kernel */
#else
    return id[4] == 2;                        /* ELFCLASS64 image on a 32-bit kernel */
#endif
}

/* ---- boot-time scan -----------------------------------------------------
 * Called once after the root filesystem is mounted.  Loads every .drv the
 * kernel reports, in directory order.
 *
 * Contract: this MUST NOT panic or block boot.  Each image is independent:
 * a bad, missing, or self-rejecting driver is logged and skipped so the
 * rest of the system still comes up.  A driver whose init() fails is left
 * in DRV_STATE_FAULTED by the loader and is simply not usable -- that is a
 * normal, recoverable outcome, not a fatal error.
 *
 * Returns the number of drivers successfully loaded. */
int driver_boot_scan(void)
{
    /* No enumeration hook (host harness) or no filesystem yet: nothing to do. */
    if (!nexos_fs_drv_count || !nexos_fs_drv_name) {
        serial_puts("[DRIVER] scan: no fs enumeration hook\n");
        return 0;
    }
    if (!kern_fs_ready()) {
        serial_puts("[DRIVER] scan: no filesystem mounted, skipping\n");
        return 0;
    }

    int total = nexos_fs_drv_count();
    if (total < 0) total = 0;
    serial_puts("[DRIVER] scan: ");
    serial_puts_dec(total);
    serial_puts(" .drv image(s) on SFS\n");

    int loaded = 0;
    for (int i = 0; i < total; ++i) {
        const char* name = nexos_fs_drv_name(i);
        if (!name || !name[0]) continue;

        /* nexos_fs_drv_name() returns a pointer INTO the filesystem's shared
         * directory buffer.  Everything below -- the log line, kern_fs_size()
         * and kern_fs_read() -- performs another directory read that overwrites
         * that buffer, so the name has to be copied out first.  Without this
         * the loader got a corrupt path (and read the wrong file); it only
         * showed up once the scan ran late enough for kmalloc to succeed,
         * because the earlier failure happened before any directory read. */
        char namebuf[DRV_MAX_PATH];
        if (str_copy(namebuf, name, DRV_MAX_PATH) <= 0) continue;

        serial_puts("[DRIVER] scanning ");
        serial_puts(namebuf);
        serial_puts("\n");
        /* Wrong ELF class for this kernel: skip before parsing anything. */
        if (image_is_other_class(namebuf)) {
            serial_puts("[DRIVER] skip ");
            serial_puts(namebuf);
            serial_puts(" (other ELF class)\n");
            continue;
        }

        /* Skip anything already loaded: a re-scan (or a driver pulled in as
         * a dependency) must not be initialised twice. */
        int rc = driver_load(namebuf);
        if (rc == DRIVER_OK) {
            ++loaded;
        } else if (rc == DRIVER_ERR_DUPLICATE) {
            serial_puts("[DRIVER] already loaded, skipping\n");
        } else {
            /* Log and degrade.  Never abort the scan: one broken image must
             * not cost the user every other driver. */
            serial_puts("[DRIVER] load failed rc=");
            serial_puts_dec(rc < 0 ? -rc : rc);
            serial_puts(" -- skipped\n");
        }
    }

    serial_puts("[DRIVER] scan complete: ");
    serial_puts_dec(loaded);
    serial_puts(" loaded\n");
    return loaded;
}

/* ---- introspection ------------------------------------------------------ */
const DriverHeader* driver_find(const char* name)
{
    DriverRec* d = find_rec(name);
    return d ? d->hdr : 0;
}

DriverStatus driver_get_status(const char* name)
{
    DriverRec* d = find_rec(name);
    if (!d) return DRIVER_ERR_NOT_FOUND;
    switch (d->state) {
    case DRV_STATE_INITED:    return DRIVER_OK;
    case DRV_STATE_LOADED:    return d->last_error;
    case DRV_STATE_FAULTED:   return d->last_error ? d->last_error
                                                  : DRIVER_ERR_INIT_FAILED;
    case DRV_STATE_UNLOADING: return DRIVER_ERR_BUSY;
    default:                  return DRIVER_ERR_NOT_FOUND;
    }
}

int driver_count(void) { return g_count; }

const char* driver_name_at(int idx)
{
    if (idx < 0 || idx >= DRV_MAX_DRIVERS) return 0;
    if (g_drivers[idx].state == DRV_STATE_EMPTY) return 0;
    return g_drivers[idx].name;
}

void driver_stats(DriverStats* out)
{
    if (!out) return;
    *out = g_stats;
}

void driver_dump(void)
{
    serial_puts("[DRIVER] ---- registry (");
    puts_dec((uint32_t)g_count);
    serial_puts(" live) ----\n");
    for (int i = 0; i < DRV_MAX_DRIVERS; ++i) {
        if (g_drivers[i].state == DRV_STATE_EMPTY) continue;
        serial_puts("  ");
        serial_puts(g_drivers[i].name);
        serial_puts(" v");
        puts_hex(g_drivers[i].hdr ? g_drivers[i].hdr->version : 0);
        serial_puts(" state=");
        puts_dec((uint32_t)g_drivers[i].state);
        serial_puts(" refs=");
        puts_dec((uint32_t)g_drivers[i].refcount);
        serial_puts("\n");
    }
    serial_puts("[DRIVER] stats: loads=");
    puts_dec(g_stats.loads);
    serial_puts(" unloads=");
    puts_dec(g_stats.unloads);
    serial_puts(" reloads=");
    puts_dec(g_stats.hot_reloads);
    serial_puts(" rollbacks=");
    puts_dec(g_stats.rollbacks);
    serial_puts(" leaks=");
    puts_dec(g_stats.leaks_detected);
    serial_puts("\n");
}

const char* driver_status_str(DriverStatus st)
{
    switch (st) {
    case DRIVER_OK:                   return "OK";
    case DRIVER_ERR_INVALID_MAGIC:    return "INVALID_MAGIC";
    case DRIVER_ERR_BAD_VERSION:      return "BAD_VERSION";
    case DRIVER_ERR_INVALID_ARG:      return "INVALID_ARG";
    case DRIVER_ERR_NO_MEMORY:        return "NO_MEMORY";
    case DRIVER_ERR_IO:               return "IO_ERROR";
    case DRIVER_ERR_SYMBOL_NOT_FOUND: return "SYMBOL_NOT_FOUND";
    case DRIVER_ERR_INIT_FAILED:      return "INIT_FAILED";
    case DRIVER_ERR_UNLOAD_FAILED:    return "UNLOAD_FAILED";
    case DRIVER_ERR_NOT_FOUND:        return "NOT_FOUND";
    case DRIVER_ERR_DUPLICATE:        return "DUPLICATE";
    case DRIVER_ERR_BUSY:             return "BUSY";
    case DRIVER_ERR_ISOLATION:        return "ISOLATION";
    case DRIVER_ERR_ROLLBACK_FAILED:  return "ROLLBACK_FAILED";
    case DRIVER_ERR_NO_SPACE:         return "NO_SPACE";
    case DRIVER_ERR_ABI_MISMATCH:     return "ABI_MISMATCH";
    case DRIVER_ERR_DEP_UNSATISFIED:  return "DEP_UNSATISFIED";
    default:                          return "UNKNOWN";
    }
}
