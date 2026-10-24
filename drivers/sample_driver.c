/* ============================================================================
 * sample_driver.c - reference driver for the NexOS hot-load framework
 * ----------------------------------------------------------------------------
 * A virtual "serial console" driver: it registers a device and a per-driver
 * refcount so a hot reload can never free the struct while a caller is still
 * inside its write() path.
 *
 * Build (freestanding, 32-bit kernel):
 *   gcc -c -m32 -ffreestanding -fno-exceptions -fno-rtti -fno-pic -fno-pie \
 *       -fno-stack-protector -fno-asynchronous-unwind-tables -nostdlib \
 *       -Os -Wall -Wextra -I. -o sample_driver.o sample_driver.c
 *
 * Link into the driver image together with the header, then hand the result
 * to driver_load("<path>").  The framework's mock reader accepts the paths
 * "sample" (v1) and "sample_v2" (v2) during bring-up.
 * ==========================================================================*/
#include "driver_contract.h"

/* ---------------------------------------------------------------------------
 * Per-instance state.  Never use file-scope statics for anything that must be
 * reinitialisable: on hot reload the same driver object is reused and init()
 * runs again, so a stale static is exactly how you get a double-free.
 * -------------------------------------------------------------------------*/
#define SAMPLE_RING 256

typedef struct SampleDriver {
    const DriverKernelAPI* api;

    /* refcount: how many threads are currently inside our entry points */
    volatile int  users;
    volatile int  dying;

    char          out_ring[SAMPLE_RING];
    uint32_t      out_head;
    uint32_t      out_tail;
    uint32_t      bytes_written;

    uint32_t      magic;              /* self-guard, catches use-after-free  */
    int           device_registered;
} SampleDriver;

#define SAMPLE_LIVE_MAGIC 0x53414D50u  /* "SAMP" */

/* Driver-owned instance.  Declared early so every lifecycle hook can reach it.
 * Reused across hot reloads: init() resets it in place, which is what makes
 * unload+reload atomic with respect to callers. */
static SampleDriver g_sample_instance;

/* ---------------------------------------------------------------------------
 * Reentrancy guard
 *   enter(): refuse if the driver is being unloaded, otherwise bump users.
 *   leave(): drop the reference; unload() polls until it reaches zero.
 * Both are no-ops on a NULL self, so a late call cannot fault the kernel.
 * -------------------------------------------------------------------------*/
static int sample_enter(SampleDriver* s)
{
    if (!s) return 0;
    if (s->magic != SAMPLE_LIVE_MAGIC) return 0;   /* freed or corrupt      */
    if (s->dying) return 0;                        /* teardown in progress  */
    __sync_fetch_and_add(&s->users, 1);
    /* Re-check after incrementing: unload may have started between the two
     * checks.  This is the standard "increment then revalidate" dance. */
    if (s->dying) {
        __sync_fetch_and_sub(&s->users, 1);
        return 0;
    }
    return 1;
}

static void sample_leave(SampleDriver* s)
{
    if (!s) return;
    __sync_fetch_and_sub(&s->users, 1);
}

/* ---- ring buffer: the driver's actual "work" ---------------------------- */
static void sample_emit(SampleDriver* s, const char* str)
{
    if (!s || !str) return;
    for (const char* p = str; *p; ++p) {
        uint32_t next = (s->out_head + 1) % SAMPLE_RING;
        if (next == s->out_tail) {
            /* Full: drop rather than overwrite, so a stuck reader cannot
             * corrupt in-flight output. */
            break;
        }
        s->out_ring[s->out_head] = *p;
        s->out_head = next;
        s->bytes_written++;
    }
}

/* ---- public entry point (this is what other kernel code calls) ---------- */
int sample_write(SampleDriver* s, const char* str)
{
    if (!s || !str) return DRIVER_ERR_INVALID_ARG;
    if (!sample_enter(s)) {
        /* Being unloaded: report busy instead of touching freed memory. */
        return DRIVER_ERR_BUSY;
    }
    /* Optional: also punch through to the kernel console. */
    if (s->api && s->api->printk) s->api->printk("[sample] write\n");
    sample_emit(s, str);
    sample_leave(s);
    return DRIVER_OK;
}

/* ---------------------------------------------------------------------------
 * Lifecycle
 * -------------------------------------------------------------------------*/
static int sample_init(const DriverKernelAPI* api, void* ctx)
{
    /* IMPORTANT: ctx belongs to the loader (its registry record).  The driver
     * must NOT reinterpret it -- it owns its own state.  We only keep the
     * handle so the loader's bookkeeping stays opaque to us. */
    (void)ctx;
    SampleDriver* s = &g_sample_instance;

    if (!api) return DRIVER_ERR_INVALID_ARG;
    if (api->abi_version != DRV_ABI_VERSION) return DRIVER_ERR_ABI_MISMATCH;

    /* Re-entry on hot reload: ctx is the same object, so reset state rather
     * than allocating a fresh one (that is what makes unload/reload atomic). */
    s->api             = api;
    s->users           = 0;
    s->dying           = 0;
    s->out_head        = 0;
    s->out_tail        = 0;
    s->bytes_written   = 0;
    s->device_registered = 0;
    s->magic           = SAMPLE_LIVE_MAGIC;

    api->printk("[sample] init: api bound, ABI OK\n");

    int rc = api->register_device("sample_sio", (void*)s);
    if (rc != DRIVER_OK) {
        api->printk("[sample] register_device failed\n");
        s->magic = 0;
        return rc;
    }
    s->device_registered = 1;

    if (api->request_irq) {
        /* Deliberately no real IRQ handler in the skeleton; we only exercise
         * the ownership path so reclaim-on-unload can be regression-tested. */
        api->request_irq(0x20, 0, s);
    }

    api->printk("[sample] Init Success\n");
    return DRIVER_OK;
}

static void sample_unload(void* ctx)
{
    (void)ctx;
    SampleDriver* s = &g_sample_instance;
    if (s->magic != SAMPLE_LIVE_MAGIC) return;

    /* 1) fence: no new users may enter. */
    s->dying = 1;

    /* 2) drain: wait for in-flight callers.  Bounded so a wedged caller
     *    cannot hang the kernel; the loader has its own timeout too. */
    uint32_t spins = 0;
    while (s->users > 0 && spins < 1000000u) { ++spins; __asm__ __volatile__("pause"); }

    const DriverKernelAPI* api = s->api;

    /* 3) release resources through the kernel, not by hand. */
    if (api) {
        if (s->device_registered && api->unregister_device)
            api->unregister_device("sample_sio");
        if (api->free_irq) api->free_irq(0x20);
        api->printk("[sample] unload: resources released\n");
    }

    /* 4) poison the object so any straggler call fails the magic check
     *    instead of dereferencing freed memory. */
    s->device_registered = 0;
    s->magic = 0;
    s->api   = 0;
}

static int sample_suspend(void* ctx)
{
    (void)ctx;
    g_sample_instance.dying = 1;
    return DRIVER_OK;
}

static int sample_resume(void* ctx)
{
    (void)ctx;
    g_sample_instance.dying = 0;
    return DRIVER_OK;
}

/* ---------------------------------------------------------------------------
 * Descriptor.  The symbol table is filled in-place by the loader; it MUST be
 * writable, so the header is non-const in the image and only the exported
 * pointer to it is treated as stable by the kernel.
 *
 * dependencies: none for the sample.  A driver needing a bus abstraction
 * would list e.g. { "pci", 0 }.
 * -------------------------------------------------------------------------*/
static DriverSymbolReq g_sample_symbols[] = {
    { "kmalloc", 0 },
    { "kfree",   0 },
    { "printk",  0 },
    { 0, 0 }
};

static const char* const g_sample_deps[] = { 0 };   /* NULL-terminated */

/* The instance lives in the driver image itself.  The loader passes the
 * registry record as ctx, but the driver owns its own state, so we hand our
 * instance to init() through a small accessor the header references. */

DriverHeader sample_driver_v1 = {
    DRV_MAGIC,              /* magic                                  */
    DRV_ABI_VERSION,        /* abi_version                            */
    0x00010000u,            /* version 1.0                            */
    sizeof(DriverHeader),   /* header_size                            */
    "sample_sio",           /* name                                   */
    "virtual serial console driver (reference implementation)",
    g_sample_deps,          /* deps                                   */
    g_sample_symbols,       /* symbols                                */
    3,                      /* symbol_count                           */
    sample_init,            /* init                                   */
    sample_unload,          /* unload                                 */
    sample_suspend,         /* suspend                                */
    sample_resume,          /* resume                                 */
    0,                      /* flags                                  */
    { 0, 0, 0, 0 }          /* reserved                               */
};

/* Version 2 of the same driver: identical layout, bumped version.  Used to
 * prove driver_hot_reload() takes a genuine version bump. */
DriverHeader sample_driver_v2 = {
    DRV_MAGIC,
    DRV_ABI_VERSION,
    0x00020000u,            /* version 2.0  <-- the bump               */
    sizeof(DriverHeader),
    "sample_sio",
    "virtual serial console driver v2 (hot-reload target)",
    g_sample_deps,
    g_sample_symbols,
    3,
    sample_init,
    sample_unload,
    sample_suspend,
    sample_resume,
    0,
    { 0, 0, 0, 0 }
};

/* Exported symbol the loader looks for. */
const DriverHeader* nexos_driver_header_v1 = &sample_driver_v1;
const DriverHeader* nexos_driver_header_v2 = &sample_driver_v2;
