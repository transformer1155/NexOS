/* ============================================================================
 * driver_contract.h - NexOS Hot-Loadable Driver Framework: contract (ABI)
 * ----------------------------------------------------------------------------
 * Pure freestanding C. No libc, no exceptions, no RTTI, no virtual.
 * Included by BOTH the kernel loader and every driver module.
 *
 * A driver image exposes exactly one exported symbol:
 *
 *     const DriverHeader nexos_driver_header;   <- named ".ndrv_header"
 *
 * The loader validates magic/ABI/version, resolves the requested kernel
 * symbols, then calls init().  unload() is guaranteed to be called before
 * the image memory is released.
 * ==========================================================================*/
#ifndef NEXOS_DRIVER_CONTRACT_H
#define NEXOS_DRIVER_CONTRACT_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ magic --
 * "NDRV" as little-endian bytes 0x4E 0x44 0x52 0x56 => 0x5652444E.
 * A driver with any other magic is rejected before any pointer is touched.  */
#define DRV_MAGIC              0x5652444Eu

/* -------------------------------------------------------------------- ABI --
 * Bump the major part whenever DriverHeader or DriverKernelAPI changes
 * layout.  A driver compiled against a different ABI is refused outright:
 * silently calling into a stale vtable is exactly the class of bug this
 * framework exists to prevent.                                              */
#define DRV_ABI_VERSION        0x00010000u   /* 1.0 */

#define DRV_MAX_NAME           32
#define DRV_MAX_PATH           128
#define DRV_MAX_SYMBOLS        32
#define DRV_MAX_DEPS           8
#define DRV_MAX_DEVICES        16
#define DRV_MAX_DRIVERS        32

/* --------------------------------------------------------------- status ---- */
typedef enum DriverStatus {
    DRIVER_OK                   =   0,
    DRIVER_ERR_INVALID_MAGIC    =  -1,  /* header->magic != DRV_MAGIC        */
    DRIVER_ERR_BAD_VERSION      =  -2,  /* driver version impossible         */
    DRIVER_ERR_INVALID_ARG      =  -3,  /* null / oversized argument         */
    DRIVER_ERR_NO_MEMORY        =  -4,  /* kmalloc failed                    */
    DRIVER_ERR_IO               =  -5,  /* image read / parse failed         */
    DRIVER_ERR_SYMBOL_NOT_FOUND =  -6,  /* a required kernel symbol is absent*/
    DRIVER_ERR_INIT_FAILED      =  -7,  /* driver init() returned < 0        */
    DRIVER_ERR_UNLOAD_FAILED    =  -8,  /* unload() -> resource leak, unsafe */
    DRIVER_ERR_NOT_FOUND        =  -9,  /* no such registered driver         */
    DRIVER_ERR_DUPLICATE        = -10,  /* name already registered           */
    DRIVER_ERR_BUSY             = -11,  /* refcount != 0, cannot unload now  */
    DRIVER_ERR_ISOLATION        = -12,  /* address-space setup failed        */
    DRIVER_ERR_ROLLBACK_FAILED  = -13,  /* hot-reload rollback itself failed */
    DRIVER_ERR_NO_SPACE         = -14,  /* registry / device table full      */
    DRIVER_ERR_ABI_MISMATCH     = -15,  /* header->abi_version != ours       */
    DRIVER_ERR_DEP_UNSATISFIED  = -16,  /* a dependency is not loaded        */
} DriverStatus;

const char* driver_status_str(DriverStatus st);

/* ---------------------------------------------------------------- states --- */
typedef enum DriverState {
    DRV_STATE_EMPTY = 0,
    DRV_STATE_LOADED,        /* image mapped + symbols bound, init not called */
    DRV_STATE_INITED,        /* init() succeeded, driver is live             */
    DRV_STATE_FAULTED,       /* init() failed or driver tripped a guard      */
    DRV_STATE_UNLOADING,     /* teardown in progress, new refs refused        */
} DriverState;

/* -------------------------------------------------------------- symbols ----
 * Kernel exports are addressed by name so a driver never links against a
 * fixed kernel address (which would break on every rebuild).                */
typedef struct DriverSymbolReq {
    const char* name;        /* e.g. "kmalloc"                          */
    void*       addr;        /* OUT: loader writes the resolved address */
} DriverSymbolReq;

/* ------------------------------------------------- kernel service vtable ---
 * Handed to init().  Every entry is non-null on a valid load.                */
typedef struct DriverKernelAPI {
    uint32_t abi_version;

    void*    (*kmalloc)(uint32_t size);
    void     (*kfree)(void* p);
    void     (*printk)(const char* s);          /* serial console          */

    int      (*register_device)(const char* name, void* ops);
    int      (*unregister_device)(const char* name);

    int      (*request_irq)(uint8_t irq, void (*handler)(void*), void* ctx);
    void     (*free_irq)(uint8_t irq);

    /* --- physical page allocator -------------------------------------
     * Hardware virtualization needs page-aligned PHYSICAL frames: the VMCB
     * and the VM_HSAVE_PA area are addressed by physical address in an MSR,
     * so kmalloc() is not sufficient.  These map to pmm_alloc_page /
     * pmm_free_page / pmm_alloc_range / pmm_free_range in kernel.cpp. */
    uint32_t (*pmm_alloc_page)(void);                 /* 4 KiB frame, PA   */
    void     (*pmm_free_page)(uint32_t pa);
    uint32_t (*pmm_alloc_range)(uint32_t pages);      /* contiguous frames */
    void     (*pmm_free_range)(uint32_t pa, uint32_t pages);

    /* CPUID leaf query: eax/ecx in, eax/ebx/ecx/edx out (all may be NULL). */
    void     (*cpuid)(uint32_t leaf, uint32_t sub,
                      uint32_t* a, uint32_t* b, uint32_t* c, uint32_t* d);

    /* --- use-count / unload-fencing --------------------------------------
     * A driver wraps every externally-visible entry point with ref_get /
     * ref_put.  unload() waits for the count to drain, so a hot reload can
     * never free memory that another context is still executing.            */
    int      (*ref_get)(void* driver_ctx);      /* 1 = ok, 0 = unloading    */
    void     (*ref_put)(void* driver_ctx);
    int      (*unload_pending)(void* driver_ctx);
} DriverKernelAPI;

/* -------------------------------------------------------------- header ---- */
typedef struct DriverHeader {
    uint32_t            magic;          /* DRV_MAGIC                       */
    uint32_t            abi_version;    /* DRV_ABI_VERSION                 */
    uint32_t            version;        /* 0x00010000 == v1.0              */
    uint32_t            header_size;    /* sizeof(DriverHeader)            */

    const char*         name;           /* unique, <= DRV_MAX_NAME         */
    const char*         description;
    const char* const*  deps;           /* NULL-terminated, or NULL        */

    DriverSymbolReq*    symbols;        /* OUT addr filled by loader       */
    uint32_t            symbol_count;   /* <= DRV_MAX_SYMBOLS              */

    int  (*init)(const DriverKernelAPI* api, void* ctx);
    void (*unload)(void* ctx);
    int  (*suspend)(void* ctx);         /* optional, may be NULL           */
    int  (*resume)(void* ctx);          /* optional, may be NULL           */

    uint32_t            flags;
    uint32_t            reserved[4];    /* keep layout stable              */
} DriverHeader;

#define DRV_FLAG_RING3          (1u << 0)   /* requires user-space isolation */
#define DRV_FLAG_IRQ_SAFE       (1u << 1)   /* may run in interrupt context  */
#define DRV_FLAG_NO_HOT_RELOAD  (1u << 2)   /* refuse live replacement       */

/* ------------------------------------------------------------ isolation ---
 * Module 4 requirement: if the kernel can give a driver its own virtual
 * address space, use it.  Today NexOS only has a single kernel address
 * space (PG_USER region reserved at USER_BASE in mm.h), so the loader
 * fills the descriptor with DRV_ISO_NONE and these hooks are the seam
 * where an ALC/paging backend would be plugged in later.                     */
typedef enum DriverIsolation {
    DRV_ISO_NONE   = 0,   /* driver runs in kernel space (current)         */
    DRV_ISO_PAGING = 1,   /* private page directory                        */
    DRV_ISO_ALC    = 2,   /* address-space layout customisation (Win ALPC) */
    DRV_ISO_RING3  = 3,   /* user-mode driver process                      */
} DriverIsolation;

typedef struct DriverAddrSpace {
    DriverIsolation kind;
    uint32_t        base;      /* VA base, 0 when NONE                     */
    uint32_t        size;
    void*           page_dir;  /* opaque, backend-owned                    */

    int  (*map)(struct DriverAddrSpace* as, uint32_t va, uint32_t pa, uint32_t flags);
    void (*unmap)(struct DriverAddrSpace* as, uint32_t va, uint32_t len);
    int  (*grant)(struct DriverAddrSpace* as, const struct DriverAddrSpace* src,
                  uint32_t va, uint32_t len);   /* cross-AS sharing         */
    void (*destroy)(struct DriverAddrSpace* as);
} DriverAddrSpace;

/* ------------------------------------------------------- loader API ------- */
int                 driver_subsystem_init(void);          /* once, at boot   */
int                 driver_load(const char* path);
int                 driver_unload(const char* name);
int                 driver_hot_reload(const char* name, const char* new_path);

const DriverHeader* driver_find(const char* name);
DriverStatus        driver_get_status(const char* name);
int                 driver_count(void);
const char*         driver_name_at(int idx);
void                driver_dump(void);                    /* serial report   */

/* Diagnostics counters, useful for the panic-recovery regression tests. */
typedef struct DriverStats {
    uint32_t loads;
    uint32_t unloads;
    uint32_t hot_reloads;
    uint32_t rollbacks;
    uint32_t leaks_detected;
    uint32_t rejected_magic;
    uint32_t rejected_symbols;
} DriverStats;
void driver_stats(DriverStats* out);

#ifdef __cplusplus
}
#endif
#endif /* NEXOS_DRIVER_CONTRACT_H */
