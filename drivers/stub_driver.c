/* ============================================================================
 * stub_driver.c - hardware-independent placeholder driver
 * ----------------------------------------------------------------------------
 * A minimal ELF32 hot-loadable driver whose init() does nothing but return 0.
 *
 * WHY THIS EXISTS
 *   The only other image on the SFS is svm.drv, whose init() needs real AMD-V
 *   (CPUID.80000001:ECX[2] + VM_CR.SVM_DISABLE clear + a working VMRUN).  No
 *   ordinary QEMU/TCG CPU models satisfy that, so on this host the boot scan
 *   can *never* report a successful load.  That makes the whole happy path --
 *   "scan finds an image, relocates it, binds it, init() succeeds, the driver
 *   becomes visible in the registry" -- untestable on ordinary hardware.
 *
 *   This driver touches no MSR, no page tables, no device: it is the smallest
 *   possible proof that the loader, the registry, the device table and the
 *   boot-time scan all work.  It deliberately declares ZERO kernel imports
 *   (everything goes through the DriverKernelAPI vtable), so it cannot fail
 *   for a symbol-binding reason either.
 *
 * CONTRACT NOTES
 *   - Exports exactly one symbol: nexos_driver_header (the loader finds an
 *     exported DriverHeader whose name begins with "nexos_driver_header").
 *   - init() and unload() must both be non-NULL: validate_header() rejects a
 *     driver that omits either.
 *   - ctx is the loader's DriverRec*, NOT ours.  All state lives in the
 *     static instance below, which init() resets in place so that a hot
 *     reload (init -> unload -> init) starts clean.
 *
 * Build (ELF32 ET_REL, per drivers/_build_drv.sh):
 *   gcc -m32 -ffreestanding -nostdlib -fno-pic -fno-pie \
 *       -fno-stack-protector -fno-asynchronous-unwind-tables -Os -Wall -Wextra \
 *       -Idrivers -c stub_driver.c -o stub_driver.o
 *   ld -m elf_i386 -r -o stub.drv stub_driver.o
 * ==========================================================================*/
#include "driver_contract.h"

#define STUB_LIVE_MAGIC 0x53545542u   /* "STUB": guards against double unload */

/* Driver-owned instance.  Allocated in the driver image itself, so a hot
 * reload gets a fresh copy; init() still resets it in place, which is what
 * makes unload+reload atomic with respect to any straggling caller. */
typedef struct StubDriver {
    const DriverKernelAPI* api;
    uint32_t magic;               /* STUB_LIVE_MAGIC while initialised        */
    int      device_registered;   /* we own "stub" in the loader's dev table  */
    uint32_t init_calls;          /* diagnostics: proves re-entry on reload   */
} StubDriver;

static StubDriver g_stub;

static void stub_puts(const char* s)
{
    if (s && g_stub.api && g_stub.api->printk) g_stub.api->printk(s);
}

/* ---------------------------------------------------------------------------
 * init: no hardware, no privileges, no allocation.  Always succeeds unless
 * the loader hands us a broken vtable or refuses the device registration.
 * -------------------------------------------------------------------------*/
static int stub_init(const DriverKernelAPI* api, void* ctx)
{
    int rc;

    /* ctx belongs to the loader (its DriverRec).  Never reinterpret it. */
    (void)ctx;

    if (!api) return DRIVER_ERR_INVALID_ARG;
    if (api->abi_version != DRV_ABI_VERSION) return DRIVER_ERR_ABI_MISMATCH;

    /* Reset in place (hot reload runs init() again on the same object). */
    g_stub.api = api;
    g_stub.magic = STUB_LIVE_MAGIC;
    g_stub.device_registered = 0;

    g_stub.init_calls++;
    if (g_stub.init_calls > 1) {
        stub_puts("[STUB] init: re-entered (hot reload path)\n");
    }
    stub_puts("[STUB] init: hardware-independent stub, no MSR/CPUID touched\n");

    /* A harmless device so the loader's device table is exercised too. */
    if (api->register_device) {
        rc = api->register_device("stub", (void*)&g_stub);
        if (rc != DRIVER_OK) {
            stub_puts("[STUB] init: register_device failed\n");
            g_stub.magic = 0;
            g_stub.api = 0;
            return rc;
        }
        g_stub.device_registered = 1;
    }

    stub_puts("[STUB] Init Success\n");
    return DRIVER_OK;
}

/* ---------------------------------------------------------------------------
 * unload: idempotent and leak-free.  Clearing magic first makes a second
 * call a no-op instead of a double unregister.
 * -------------------------------------------------------------------------*/
static void stub_unload(void* ctx)
{
    (void)ctx;

    if (g_stub.magic != STUB_LIVE_MAGIC) return;   /* never init'd / already off */
    g_stub.magic = 0;

    if (g_stub.device_registered && g_stub.api && g_stub.api->unregister_device) {
        g_stub.api->unregister_device("stub");
    }
    g_stub.device_registered = 0;

    stub_puts("[STUB] unload: device released\n");
    g_stub.api = 0;
}

/* ---------------------------------------------------------------------------
 * Descriptor.  symbol_count is 0 on purpose: this driver needs nothing from
 * the kernel export table, so it cannot be rejected by symbol binding.  deps
 * is NULL: nothing has to be loaded before us.
 * -------------------------------------------------------------------------*/
DriverHeader nexos_driver_header = {
    .magic        = DRV_MAGIC,
    .abi_version  = DRV_ABI_VERSION,
    .version      = 0x00010000u,          /* 1.0                            */
    .header_size  = sizeof(DriverHeader),
    .name         = "stub",
    .description  = "hardware-independent stub driver (load-path smoke test)",
    .deps         = 0,                    /* no dependencies                */
    .symbols      = 0,                    /* no kernel imports              */
    .symbol_count = 0,
    .init         = stub_init,
    .unload       = stub_unload,
    .suspend      = 0,
    .resume       = 0,
    .flags        = 0,
    .reserved     = { 0, 0, 0, 0 }
};
