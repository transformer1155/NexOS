// =====================================================================
//  nexos_compat.h -- AI-assisted Win32 compatibility layer, kernel side
// ---------------------------------------------------------------------
//  PURPOSE
//    The kernel does NOT run any AI.  An EXTERNAL agent (on the host) watches
//    a PE program through a tiny file protocol on the MKFS data volume and
//    feeds back mappings / fallbacks.  This header is the contract between
//    the two.
//
//  CHANNEL (decision 2026-11-13)
//    MKFS data volume, double buffered, sequence numbered:
//        ai_events.bin / ai_events.prev   kernel -> AI  (events + health)
//        ai_cmd.bin   / ai_cmd.prev       AI -> kernel  (commands)
//    No inbound network is used.  The volume is build/data_vbox.vdi, which
//    the host can mount.
//
//  SECURITY (decision 2026-11-13)
//    The AI may NOT upload machine code.  It may only:
//      - select an already-compiled-in kernel implementation ("approx"),
//      - ask for a dummy/stub return,
//      - toggle observation, fault policy, and resume budget.
//    Every patch therefore rebinds an IAT slot to an EXISTING kernel
//    function pointer -- there is never a ring-0 code injection path.
//
//  SCHEDULING
//    Everything here is cooperative.  compat_pump() is called once per frame
//    from the paint overlay, with a work budget; it never spins, never
//    blocks, and never waits for the host.
// =====================================================================
#ifndef NEXOS_COMPAT_H
#define NEXOS_COMPAT_H

#include <stdint.h>

// ---------------------------------------------------------------------
//  File protocol
// ---------------------------------------------------------------------
#define COMPAT_MAGIC      0x4D4F4350u   /* 'PCOM' little-endian */
#define COMPAT_VERSION    1u

#define COMPAT_FILE_EVENTS 1u
#define COMPAT_FILE_CMDS   2u

// Header is 24 bytes; payload follows immediately.
typedef struct {
    uint32_t magic;      // COMPAT_MAGIC
    uint16_t version;    // COMPAT_VERSION
    uint16_t kind;       // COMPAT_FILE_*
    uint32_t seq;        // monotonically increasing; reader takes the larger
    uint32_t len;        // payload byte count
    uint32_t crc;        // crc32 of payload; reader skips a torn write
    uint32_t reserved;
} CompatFileHeader;

// Command opcodes (AI -> kernel).
enum {
    COMPAT_OP_PATCH        = 1,  // bind an IAT slot to a builtin impl
    COMPAT_OP_FAULT_MODE   = 2,  // flags bit0: 1 = aggressive (skip) mode
    COMPAT_OP_OBSERVE      = 3,  // arg0: 1 = on, 0 = off
    COMPAT_OP_RESUME       = 4,  // arg0: extra api-call budget to grant
    COMPAT_OP_EXPERIMENTAL = 5,  // flags bit0: mark next patch experimental
    COMPAT_OP_ACK          = 6   // arg0: ack for cmd seq (no-op today)
};

// Patch actions (a[2] of the patch command).
enum {
    COMPAT_ACT_EXACT    = 0,  // bind the exact builtin implementation
    COMPAT_ACT_APPROX   = 1,  // bind a builtin "close enough" implementation
    COMPAT_ACT_DUMMY    = 2,  // bind the shared dummy-return stub
    COMPAT_ACT_UNDEGRADE= 3   // restore the exact implementation
};

// A command record is FIXED SIZE so the host can generate it without a
// string library and the kernel can parse it without one.
#define COMPAT_CMD_STR0 32
#define COMPAT_CMD_STR1 16
typedef struct {
    uint16_t op;         // COMPAT_OP_*
    uint16_t flags;      // bit0: experimental patch
    uint32_t arg0, arg1; // opcode-specific
    char     s0[COMPAT_CMD_STR0];  // "kernel32!GetTickCount64"
    char     s1[COMPAT_CMD_STR1];  // "GetTickCount"
} CompatCmd;            // 64 bytes

// ---------------------------------------------------------------------
//  Event ring (kernel -> AI)
// ---------------------------------------------------------------------
// One format for everything, discriminated by `kind`.  The health sampler
// and the API tracer share the ring so the host reads ONE file.
#define COMPAT_EV_API      1   // an API call was observed
#define COMPAT_EV_FAULT    2   // a CPU fault was taken
#define COMPAT_EV_PATCH    3   // an IAT slot was rebound
#define COMPAT_EV_ROLLBACK 4   // an experimental patch was rolled back
#define COMPAT_EV_HEALTH   5   // periodic health sample
#define COMPAT_EV_LEAK     6   // leak sentinel verdict
#define COMPAT_EV_SUMMARY  7   // quality summary
#define COMPAT_EV_NOTE     8   // free-form note

#define COMPAT_RING        256
#define COMPAT_EV_STR      40

typedef struct {
    uint32_t seq;        // monotonic
    uint16_t kind;       // COMPAT_EV_*
    uint16_t name_id;    // IAT slot -> name pool index, or 0xFFFF
    uint32_t slot;       // IAT slot index, or metric id
    uint32_t a0, a1, a2; // api args, or metric values
    uint32_t ret;        // api return value
    uint32_t ts;         // osapi_now_ms()
    uint8_t  status;     // see COMPAT_ST_*
    uint8_t  pad;
    char     s[COMPAT_EV_STR];  // diagnosis text / reason / suggestion
} CompatEvent;          // 64 bytes

// status codes
#define COMPAT_ST_OK       0
#define COMPAT_ST_STUB     1   // served by the dummy/stub path
#define COMPAT_ST_FAIL     2   // api reported failure
#define COMPAT_ST_FAULT    3   // fault taken during this call
#define COMPAT_ST_ROLLBACK 4   // patch rolled back
#define COMPAT_ST_LEAK     5   // leak suspected

// ---------------------------------------------------------------------
//  IAT slot reverse index
// ---------------------------------------------------------------------
// The loader already knows every (dll, function, IAT address) triple at
// import-resolution time.  Recording it ONCE gives us:
//   - reverse lookup  slot -> api name      (what the AI needs)
//   - a patchable address                  (what D2 needs)
//   - a place to hang a per-slot flag set
#define COMPAT_IAT_MAX     256
#define COMPAT_NAME_MAX    256
#define COMPAT_NAME_LEN    40

#define COMPAT_F_OBSERVED  0x01  // slot is being traced
#define COMPAT_F_DEGRADED  0x02  // slot currently bound to a stub
#define COMPAT_F_EXPERIMENT 0x04 // slot bound by an experimental patch
#define COMPAT_F_ROLLED    0x08  // experimental patch was rolled back

typedef struct {
    uint64_t addr;       // absolute address of the IAT slot
    void*    impl;       // current binding
    void*    prev_impl;  // binding to restore on rollback
    uint32_t name_id;    // index into the name pool
    uint16_t dll_id;     // index into the name pool (dll part)
    uint16_t flags;      // COMPAT_F_*
    uint32_t exp_faults; // faults seen since an experimental patch
    uint32_t exp_calls;  // calls seen since an experimental patch
    uint32_t exp_fails;  // failures seen since an experimental patch
} CompatIatSlot;

typedef struct {
    uint64_t addr;       // IAT slot address
    uint64_t impl;       // current implementation
    uint32_t name_id;
    uint32_t ts;
    uint32_t calls;
    uint32_t fails;
    uint32_t faults;
} CompatIatSnapshot;    // 32 bytes

// ---------------------------------------------------------------------
//  PE execution context (D0: the cooperative fiber)
// ---------------------------------------------------------------------
// win32_run() used to call the PE entry synchronously, which froze the
// whole frame loop for the program's lifetime -- so the AI could never
// observe anything mid-run.  The PE now runs on its OWN stack and yields
// back to the kernel at API call boundaries.
#define PE_STATE_IDLE      0
#define PE_STATE_RUNNING   1
#define PE_STATE_EXITED    2
#define PE_STATE_ABORTED   3   // ended by a fault (conservative mode)
#define PE_STATE_FAILED    4   // load failed before it ever ran

typedef struct {
    uint64_t rbx, rbp, r12, r13, r14, r15;  // callee-saved
    uint64_t rsp;      // stack pointer at the yield point
    uint64_t rip;      // resume address
    uint64_t retval;   // PE exit code
    uint64_t* stack;   // the PE's own stack (pmm pages)
    uint64_t image_base;
    uint64_t entry_rva;
    uint32_t state;    // PE_STATE_*
    uint32_t api_calls;// API calls made so far
    uint32_t budget;   // calls allowed before the next yield
    uint32_t faulted;  // a fault aborted this PE
    uint32_t session;  // session id handed back to the caller
    uint32_t started;  // 1 once the fiber has been entered at least once
    uint32_t pending_ret; // PE entry's return value
    int32_t  last_slot;// IAT slot of the most recent API call
} PeContext;

#define COMPAT_PE_MAX      4       // concurrent PE sessions
#define COMPAT_STACK_SIZE  (128*1024)

// ---------------------------------------------------------------------
//  Diagnostics (E2): rule table
// ---------------------------------------------------------------------
// The AI should not have to re-derive "page fault inside the PE stack means
// the stack ran out".  Each rule maps a fault signature to a probable cause
// and a suggested action; the first matching rule wins.
typedef struct {
    uint32_t vec_mask;    // required exception vector (0xffffffff = any)
    uint32_t vec_val;     // value masked by vec_mask
    uint32_t cr2_lo;      // CR2 must be >= this (0 = don't care)
    uint32_t cr2_hi;      // CR2 must be <= this (0xffffffff = don't care)
    uint8_t  in_pe;       // 1 = fault address must be inside a PE stack
    const char* cause;    // probable cause (static string)
    const char* action;   // suggested action for the AI
} CompatDiagRule;

// ---------------------------------------------------------------------
//  Leak sentinel (E3)
// ---------------------------------------------------------------------
#define COMPAT_SNAP_MAX     4    // ring of snapshots
#define COMPAT_SNAP_INTERVAL 1000// frames between snapshots
#define COMPAT_LEAK_MIN_GROWTH 4  // net growth over the window that counts

typedef struct {
    uint32_t frame;
    uint32_t hcount[24];  // live handles per HK_* kind (index 0..23)
    uint32_t iat_used;    // number of live IAT slots
    uint32_t calls;
    uint32_t fails;
} CompatSnapshot;         // 104 bytes

// ---------------------------------------------------------------------
//  Health metrics (E1)
// ---------------------------------------------------------------------
typedef struct {
    uint32_t frame;
    uint32_t ts;
    uint32_t tick_ms;      // scheduler latency since the previous tick
    uint32_t hcount[24];   // live handles per kind
    uint32_t heap_bytes;   // kmalloc bytes in use
    uint32_t heap_allocs;  // live kmalloc count
    uint32_t pmm_free;     // free physical pages
    uint32_t pe_active;    // live PE sessions
    uint32_t calls;        // cumulative API calls
    uint32_t fails;        // cumulative API failures
    uint32_t stubs;        // cumulative degraded calls
    uint32_t patches;      // live patch count
    uint32_t leaks;        // leak verdicts so far
    uint32_t rollbacks;    // rollbacks so far
} CompatHealth;            // 128 bytes

// ---------------------------------------------------------------------
//  Quality summary (E5)
// ---------------------------------------------------------------------
typedef struct {
    uint32_t ts;
    uint32_t pe_active;
    uint32_t calls;
    uint32_t fails;
    uint32_t stubs;
    uint32_t patches;
    uint32_t rollbacks;
    uint32_t top[3];       // IAT slot indices of the 3 worst APIs
    uint32_t top_fails[3]; // their failure counts
    uint32_t fail_rate_pct;
    uint32_t degrade_pct;
} CompatSummary;           // 64 bytes

// ---------------------------------------------------------------------
//  Public API
// ---------------------------------------------------------------------
// Everything below is declared with C linkage in the consolidated
// extern "C" block further down, so the kernel, osapi.cpp and the win32
// loader can all call it without worrying about name mangling.

// --- lifecycle -------------------------------------------------------
// compat_init():        arm the layer (allocate the dummy pool, clear state)
// compat_pump(budget):  called once per frame; reads the AI's commands,
//                        advances the PE, samples health, publishes events.
//                        NEVER blocks; every loop is bounded by budget.
// compat_iat_register(): called by the PE loader once per imported symbol
// compat_iat_find():    reverse lookup: (dll, fn) -> slot index
// compat_iat_name():    slot -> "dll!fn"
// compat_patch_iat():   rebind one IAT slot (the hot patch)
// compat_dummy_return(): the shared target every degraded API funnels into
// compat_dummy_alloc(): a writable dummy block for a degraded VirtualAlloc
// compat_note_call():   called by the trampoline around each observed call
// compat_on_fault():    called by fault_common() with the fault details
// compat_abort_pe():    longjmp out of a faulting PE, back into the kernel

// --- plumbing used by other translation units -------------------------
// C linkage: nexos_compat.cpp defines these, osapi.cpp and the kernel
// call them, and the kernel links with --unresolved-symbols=ignore-all
// (a C++-mangled definition would silently bind to 0).
extern "C" {
uint64_t compat_now_ms(void);
void     compat_event(uint16_t kind, uint16_t name_id, uint32_t slot,
                      uint32_t a0, uint32_t a1, uint32_t a2, uint32_t ret,
                      uint8_t status, const char* text);
void     compat_summary_text(char* out, int cap);
void     compat_fault_text(char* out, int cap);
void     compat_note_call(int slot, uint32_t a0, uint32_t a1, uint32_t a2,
                          uint32_t ret, uint8_t status);
void     compat_observe_set(int on);
void     compat_on_fault(uint32_t vec, uint32_t err, uint64_t rip, uint64_t cr2);
void     compat_abort_pe(void);
void     compat_init(void);
int      compat_pump(int budget);
int      compat_iat_register(uint64_t slot_addr, const char* dll,
                             const char* fn, void* impl);
int      compat_iat_find(const char* dll, const char* fn);
const char* compat_iat_name(int slot);
void*    compat_iat_impl(int slot);
int      compat_iat_count(void);
int      compat_patch_iat(const char* dll, const char* fn,
                          int action, int sel, int experimental);
int      compat_patch_count(void);
void     compat_set_fault_aggressive(int on);
void*    compat_dummy_return(void);
uint64_t compat_dummy_alloc(uint32_t size);
int      compat_dummy_handle(void);
int      compat_pe_active(void);
uint32_t compat_pe_session(void);
int      compat_pe_start(uint64_t entry_rva, uint64_t image_base);
int      compat_pe_resume(int budget);
void     compat_health_sample(void);
void     compat_leak_check(void);
void     compat_summary(void);
int      compat_diag_lookup(uint32_t vec, uint64_t cr2, int in_pe,
                            const char** cause, const char** action);

// The kernel hands the compat layer its two volume hooks: how to read a
// file from the MKFS data volume (the AI's command channel) and how to
// write one (the AI's event channel).
typedef int  (*CompatReadHook)(const char* name, void* buf, int cap);
typedef int  (*CompatWriteHook)(const char* name, const unsigned char* data,
                                 int len);
void compat_attach_volume(CompatReadHook rd, CompatWriteHook wr);
}

// --- 64-bit setjmp / longjmp (nexos_compat_asm.S) --------------------
typedef struct {
    uint64_t rbx, rbp, r12, r13, r14, r15;
    uint64_t rsp, rip;
} CompatCtx;

extern "C" int  compat_setjmp(CompatCtx* c);
extern "C" void compat_longjmp(CompatCtx* c, int val);
extern "C" void compat_fiber_switch(CompatCtx* save, uint64_t new_rsp,
                                    uint64_t new_rip);

#endif // NEXOS_COMPAT_H
