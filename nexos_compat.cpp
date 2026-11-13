// =====================================================================
//  nexos_compat.cpp -- AI-assisted Win32 compatibility layer, kernel side.
//
//  See nexos_compat.h for the contract.  The short version:
//
//    * The kernel runs NO AI.  An external agent watches the PE through
//      a file protocol on the MKFS data volume and feeds back decisions.
//    * The PE runs COOPERATIVELY on its own stack (D0), so the frame loop
//      keeps turning and the AI can observe it live.
//    * A CPU fault inside the PE (D4) is turned into a report plus a
//      longjmp back into the kernel -- the machine survives.
//    * Every API call can be observed (D1), rebound to a different
//      already-compiled-in implementation (D2), or degraded to a dummy
//      return (D3).
//    * An APMS-style operations layer rides on top (E1-E5): health
//      sampling, a diagnosis rule table, a leak sentinel, experimental
//      patch rollback, and a periodic quality summary.
//
//  NOTHING here blocks.  compat_pump() is called once per frame with a
//  work budget; every loop in this file is bounded by that budget.
// =====================================================================
#include "nexos_compat.h"
#include "osapi.h"

// ---- kernel services we borrow (all extern "C" in the kernel) ---------
extern "C" void*  kmalloc(uint32_t size);
extern "C" void   kfree(void* ptr);
extern "C" uint32_t pmm_alloc_page(void);
extern "C" void   pmm_free_page(uint32_t phys);
extern "C" uint32_t pmm_alloc_range(uint32_t count);
extern "C" void   pmm_free_range(uint32_t pa, uint32_t pages);
extern "C" int    kern_fs_create(const char* name, const unsigned char* data,
                                 int len);
extern "C" void   apms_event(int code, const char* tag, const char* msg);

// The MKFS volume is a file-static inside kernel64.cpp; we reach it through
// these hooks, which the kernel installs at boot.
static CompatReadHook  g_vol_read  = 0;
static CompatWriteHook g_vol_write = 0;

extern "C" void compat_attach_volume(CompatReadHook rd, CompatWriteHook wr){
    g_vol_read  = rd;
    g_vol_write = wr;
}

// ---- win32 side hooks (set by win32.cpp) ------------------------------
// The loader tells us where every IAT slot lives; we tell it what to bind.
// Storage for the resolver pointer (installed by compat_set_resolver at
// startup).  Defined here so the symbol resolves; until the loader installs
// a real resolver the layer degrades gracefully (compat_set_resolver /
// compat_patch_iat treat a null pointer as "no builtin for this slot").
extern "C" void* (*g_compat_resolve_exact)(const char* dll, const char* fn) = 0;

// The loader installs its resolver here at startup.  Resolving by name is
// what lets the AI rebind a slot to a builtin implementation: the answer
// is always a pointer to code that is already in the binary.
extern "C" void* compat_resolve_builtin(const char* dll, const char* fn);

extern "C" void compat_set_resolver(void* (*fn)(const char*, const char*)){
    g_compat_resolve_exact = fn;
}

// =====================================================================
//  Small local helpers
// =====================================================================
extern "C" uint64_t compat_now_ms(void){
    // Same calibration as osapi_now_ms() (rdtsc >> 10), so timestamps from
    // the two subsystems are directly comparable.
    uint32_t t;
    __asm__ __volatile__("rdtsc" : "=a"(t) :: "edx");
    return (uint64_t)(t >> 10);
}

static int c_len(const char* s){ int n = 0; if (!s) return 0; while (s[n]) n++; return n; }
static void c_cpy(char* d, int cap, const char* s){
    if (!d || cap <= 0) return;
    int i = 0;
    if (s) for (; s[i] && i < cap - 1; i++) d[i] = s[i];
    d[i] = 0;
}
static int  c_eq(const char* a, const char* b){
    if (!a || !b) return 0;
    while (*a && *b){ if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}
static uint32_t crc32_of(const void* data, uint32_t len){
    const uint8_t* p = (const uint8_t*)data;
    uint32_t c = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < len; i++){
        c ^= p[i];
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(c & 1)));
    }
    return ~c;
}

// =====================================================================
//  Global state
// =====================================================================
static CompatIatSlot  g_iat[COMPAT_IAT_MAX];
static int            g_iat_n;
static char           g_names[COMPAT_NAME_MAX][COMPAT_NAME_LEN];
static int            g_names_n;

static CompatEvent    g_ring[COMPAT_RING];
static int            g_ring_head;      // next write slot
static int            g_ring_count;     // live entries
static uint32_t       g_event_seq;

static PeContext      g_pe[COMPAT_PE_MAX];
static uint32_t       g_pe_next_session = 1;
static int            g_pe_cur = -1;    // index of the running PE

// The kernel-side context of the frame loop, used to switch back into it.
// g_kernel_ctx is the kernel's checkpoint; g_pe_ctx is the PE's parked ctx.
static CompatCtx      g_kernel_ctx;
static CompatCtx*     g_kernel_ctxp = &g_kernel_ctx;
static CompatCtx      g_pe_ctx;
// 1 only while PE code is actually executing on the PE stack.  Guards
// compat_abort_pe() so a fault in the *kernel* frame loop (with a PE
// merely suspended) does not wrongly longjmp back into the PE.
static int           g_in_pe_exec = 0;

// Fault bookkeeping
typedef struct {
    uint32_t valid;
    uint32_t vec, err;
    uint64_t rip, cr2;
    uint32_t aggressive;
    uint32_t in_pe;
    char     cause[48];
    char     action[48];
} CompatFaultState;
static CompatFaultState g_fault;

// Health / summary
static CompatHealth   g_health;
static CompatSummary  g_summary;
static uint32_t       g_frame;
static uint32_t       g_last_tick_ms;
static uint32_t       g_total_calls, g_total_fails, g_total_stubs;
static uint32_t       g_total_patches, g_total_rollbacks, g_total_leaks;

// Leak sentinel
static CompatSnapshot g_snaps[COMPAT_SNAP_MAX];
static int            g_snap_n, g_snap_head;
static uint32_t       g_snap_next_frame;

// File protocol
static uint32_t       g_cmd_seq_seen;
static uint32_t       g_evt_seq_written;
static int            g_observe_on;

// Dummy (stub) resources
static uint8_t*       g_dummy_pool;
static uint32_t       g_dummy_used;

// Observation counters per IAT slot
static uint32_t       g_slot_calls[COMPAT_IAT_MAX];
static uint32_t       g_slot_fails[COMPAT_IAT_MAX];

// The dummy stub target.  The AI is never allowed to upload code, so every
// degraded API funnels into this ONE function and the slot identity is
// recovered from the recorded context instead of from the return address.
static uint32_t g_dummy_ret;

// =====================================================================
//  IAT registry
// =====================================================================
extern "C" int compat_iat_count(void){ return g_iat_n; }

static int name_intern(const char* s){
    for (int i = 0; i < g_names_n; i++) if (c_eq(g_names[i], s)) return i;
    if (g_names_n >= COMPAT_NAME_MAX) return 0;
    c_cpy(g_names[g_names_n], COMPAT_NAME_LEN, s);
    return g_names_n++;
}

extern "C" int compat_iat_register(uint64_t slot_addr, const char* dll, const char* fn,
                         void* impl){
    if (g_iat_n >= COMPAT_IAT_MAX) return -1;
    char full[COMPAT_NAME_LEN];
    int i = 0;
    if (dll) for (; dll[i] && i < 20; i++) full[i] = dll[i];
    full[i] = 0;
    int j = c_len(full);
    if (j < COMPAT_NAME_LEN - 1){ full[j++] = '!'; }
    if (fn) for (int k = 0; fn[k] && j < COMPAT_NAME_LEN - 1; k++) full[j++] = fn[k];
    full[j] = 0;

    CompatIatSlot* s = &g_iat[g_iat_n];
    s->addr       = slot_addr;
    s->impl       = impl;
    s->prev_impl  = impl;
    s->name_id    = (uint32_t)name_intern(full);
    s->dll_id     = (uint16_t)name_intern(dll ? dll : "?");
    s->flags      = 0;
    s->exp_faults = 0;
    s->exp_calls  = 0;
    s->exp_fails  = 0;
    return g_iat_n++;
}

extern "C" int compat_iat_find(const char* dll, const char* fn){
    char full[COMPAT_NAME_LEN];
    int i = 0;
    if (dll) for (; dll[i] && i < 20; i++) full[i] = dll[i];
    full[i] = 0;
    int j = c_len(full);
    if (j < COMPAT_NAME_LEN - 1) full[j++] = '!';
    if (fn) for (int k = 0; fn[k] && j < COMPAT_NAME_LEN - 1; k++) full[j++] = fn[k];
    full[j] = 0;
    int id = -1;
    for (int n = 0; n < g_names_n; n++) if (c_eq(g_names[n], full)){ id = n; break; }
    if (id < 0) return -1;
    for (int k = 0; k < g_iat_n; k++) if (g_iat[k].name_id == (uint32_t)id) return k;
    return -1;
}

extern "C" const char* compat_iat_name(int slot){
    if (slot < 0 || slot >= g_iat_n) return "?";
    return g_names[g_iat[slot].name_id];
}

extern "C" void* compat_iat_impl(int slot){
    if (slot < 0 || slot >= g_iat_n) return 0;
    return g_iat[slot].impl;
}

// =====================================================================
//  Event ring
// =====================================================================
extern "C" void compat_event(uint16_t kind, uint16_t name_id, uint32_t slot,
                  uint32_t a0, uint32_t a1, uint32_t a2, uint32_t ret,
                  uint8_t status, const char* text){
    CompatEvent* e = &g_ring[g_ring_head];
    g_ring_head = (g_ring_head + 1) % COMPAT_RING;
    if (g_ring_count < COMPAT_RING) g_ring_count++;
    e->seq     = ++g_event_seq;
    e->kind    = kind;
    e->name_id = name_id;
    e->slot    = slot;
    e->a0 = a0; e->a1 = a1; e->a2 = a2;
    e->ret  = ret;
    e->ts   = compat_now_ms();
    e->status = status;
    e->pad  = 0;
    c_cpy(e->s, COMPAT_EV_STR, text ? text : "");
}

// =====================================================================
//  Dummy resources (D3)
// =====================================================================
#define COMPAT_DUMMY_SLOTS 8
#define COMPAT_DUMMY_SIZE  4096

extern "C" void* compat_dummy_return(void){
    // The AI may not inject code, so every degraded API lands here.  We
    // cannot tell which API we are serving from the return address alone
    // (all degraded slots share this function), so the caller records the
    // slot in the observation ring before calling us.
    g_dummy_ret = 0;
    return 0;
}

extern "C" uint64_t compat_dummy_alloc(uint32_t size){
    if (!g_dummy_pool) return 0;
    uint32_t n = (size + COMPAT_DUMMY_SIZE - 1) & ~(uint32_t)(COMPAT_DUMMY_SIZE - 1);
    if (n == 0) n = COMPAT_DUMMY_SIZE;
    if (n > COMPAT_DUMMY_SIZE) return 0;   // one dummy page per allocation
    if (g_dummy_used >= COMPAT_DUMMY_SLOTS) return 0;
    uint8_t* p = g_dummy_pool + g_dummy_used * COMPAT_DUMMY_SIZE;
    g_dummy_used++;
    // Zero it so a PE that memsets it behaves sanely.
    for (uint32_t i = 0; i < COMPAT_DUMMY_SIZE; i++) p[i] = 0;
    return (uint64_t)(uintptr_t)p;
}

extern "C" int compat_dummy_handle(void){
    // A stub handle the PE can carry around.  It is never resolved for
    // anything real, but it is non-zero so PE code that does not check
    // its return value keeps running.
    return 0x00E0;
}

// =====================================================================
//  Diagnosis rules (E2)
// =====================================================================
// The AI should not have to re-derive "page fault inside the PE stack
// means the stack ran out".  Each rule maps a fault signature to a cause
// and a suggested action; the first match wins.
static const CompatDiagRule g_diag_rules[] = {
    { 0xFFFFFFFFu, 14, 0, 0xFFFFFFFFu, 1,
      "page fault inside the PE stack: stack overflow or bad frame",
      "check VirtualAlloc stub / raise PE stack size" },
    { 0xFFFFFFFFu, 14, 0, 0xFFFFFFFFu, 0,
      "page fault outside the PE stack: null or wild pointer",
      "check the last observed API and its arguments" },
    { 0xFFFFFFFFu, 13, 0, 0xFFFFFFFFu, 0,
      "general protection fault: bad addressing mode or stack misuse",
      "inspect the last observed API; check for a missing prototype stub" },
    { 0xFFFFFFFFu, 8,  0, 0xFFFFFFFFu, 0,
      "double fault: the kernel could not handle the first fault",
      "check IST setup and the fault stack" },
    { 0, 0, 0, 0, 0, 0, 0 }   // terminator
};

extern "C" int compat_diag_lookup(uint32_t vec, uint64_t cr2, int in_pe,
                       const char** cause, const char** action){
    for (int i = 0; g_diag_rules[i].cause; i++){
        const CompatDiagRule* r = &g_diag_rules[i];
        if (r->vec_mask && (vec & r->vec_mask) != (r->vec_val & r->vec_mask)) continue;
        if (r->in_pe && !in_pe) continue;
        if (r->cr2_lo && cr2 < r->cr2_lo) continue;
        if (r->cr2_hi != 0xFFFFFFFFu && cr2 > r->cr2_hi) continue;
        if (cause)  *cause  = r->cause;
        if (action) *action = r->action;
        return 1;
    }
    if (cause)  *cause  = "unclassified fault";
    if (action) *action = "escalate to the language model";
    return 0;
}

// =====================================================================
//  Fault handling (D4)
// =====================================================================
extern "C" void compat_on_fault(uint32_t vec, uint32_t err, uint64_t rip, uint64_t cr2){
    g_fault.valid = 1;
    g_fault.vec = vec;
    g_fault.err = err;
    g_fault.rip = rip;
    g_fault.cr2 = cr2;

    // Is the fault address inside a PE stack?
    int in_pe = 0;
    for (int i = 0; i < COMPAT_PE_MAX; i++){
        if (g_pe[i].state != PE_STATE_RUNNING) continue;
        uint64_t lo = g_pe[i].rsp;
        uint64_t hi = lo + COMPAT_STACK_SIZE;
        if (cr2 >= lo && cr2 < hi){ in_pe = 1; break; }
    }
    g_fault.in_pe = (uint32_t)in_pe;

    const char* cause = 0; const char* action = 0;
    compat_diag_lookup(vec, cr2, in_pe, &cause, &action);
    c_cpy(g_fault.cause,  sizeof(g_fault.cause),  cause);
    c_cpy(g_fault.action, sizeof(g_fault.action), action);

    compat_event(COMPAT_EV_FAULT, 0xFFFF, g_pe_cur, vec, err,
                 (uint32_t)(cr2 & 0xFFFFFFFFu), (uint32_t)(rip & 0xFFFFFFFFu),
                 COMPAT_ST_FAULT, cause);

    // Mark the running PE as faulted so the rollback watchdog can act.
    if (g_pe_cur >= 0 && g_pe_cur < COMPAT_PE_MAX){
        g_pe[g_pe_cur].faulted = 1;
        CompatIatSlot* s = &g_iat[g_pe[g_pe_cur].last_slot];
        if (s->flags & COMPAT_F_EXPERIMENT) s->exp_faults++;
    }
}

extern "C" void compat_abort_pe(void){
    // Leave the faulting PE for good and re-enter the kernel frame loop.
    // compat_longjmp restores the callee-saved registers and the stack
    // pointer captured at the pump's checkpoint, so the kernel continues
    // with a consistent state and the machine stays alive.
    //
    // Only do this when we are genuinely inside a PE run; a fault in the
    // kernel frame loop (a PE merely suspended) must NOT longjmp here -- it
    // would corrupt the kernel.
    if (g_in_pe_exec && g_pe_cur >= 0 && g_pe_cur < COMPAT_PE_MAX &&
        g_pe[g_pe_cur].state == PE_STATE_RUNNING){
        g_pe[g_pe_cur].state = PE_STATE_ABORTED;
        g_pe[g_pe_cur].faulted = 1;
        g_in_pe_exec = 0;
        compat_longjmp(g_kernel_ctxp, 1);
    }
    // Not inside a PE run: nothing safe to do; the caller parks the machine.
}

// =====================================================================
//  Patching (D2)
// =====================================================================
extern "C" int compat_patch_count(void){ return (int)g_total_patches; }

// Builtin "close enough" implementations the AI may choose from.  These
// are real kernel functions, already compiled in -- no code is uploaded.
struct CompatApprox { const char* dll; const char* fn; const char* alt; };
static const CompatApprox g_approx[] = {
    { "kernel32", "GetSystemTimeAsFileTime", "GetSystemTime" },
    { "kernel32", "GetTickCount64",          "GetTickCount" },
    { "kernel32", "GetUserDefaultLCID",      "GetACP" },
    { "kernel32", "GetNativeSystemInfo",     "GetSystemInfo" },
    { "kernel32", "GetModuleFileNameW",      "GetModuleFileNameA" },
    { "kernel32", "QueryPerformanceCounter", "GetTickCount" },
    { "kernel32", "GetTimeZoneInformation",  "GetSystemTime" },
    { 0, 0, 0 }
};

extern "C" int compat_patch_iat(const char* dll, const char* fn,
                     int action, int sel, int experimental){
    int idx = compat_iat_find(dll, fn);
    if (idx < 0){ osapi_set_last_error(ERR_NOENTRY); return 0; }
    CompatIatSlot* s = &g_iat[idx];

    void* impl = 0;
    const char* alt = 0;

    switch (action){
    case COMPAT_ACT_EXACT:
        impl = g_compat_resolve_exact ? g_compat_resolve_exact(dll, fn) : 0;
        break;
    case COMPAT_ACT_APPROX:
        for (int i = 0; g_approx[i].dll; i++){
            if (c_eq(g_approx[i].dll, dll) && c_eq(g_approx[i].fn, fn)){
                alt = g_approx[i].alt;
                break;
            }
        }
        if (!alt){ osapi_set_last_error(ERR_NOTSUPP); return 0; }
        impl = g_compat_resolve_exact ? g_compat_resolve_exact(dll, alt) : 0;
        break;
    case COMPAT_ACT_DUMMY:
        impl = (void*)compat_dummy_return;
        break;
    case COMPAT_ACT_UNDEGRADE:
        impl = s->prev_impl;
        break;
    default:
        osapi_set_last_error(ERR_BADARG);
        return 0;
    }
    if (!impl){ osapi_set_last_error(ERR_NOTSUPP); return 0; }

    // Remember the previous binding so an experimental patch can be undone.
    if (!(s->flags & COMPAT_F_EXPERIMENT)) s->prev_impl = s->impl;

    // ---- the hot patch itself: rebind one IAT slot ----
    *(uint64_t*)s->addr = (uint64_t)(uintptr_t)impl;
    s->impl = impl;

    if (action == COMPAT_ACT_DUMMY) s->flags |= COMPAT_F_DEGRADED;
    else                            s->flags &= ~COMPAT_F_DEGRADED;

    if (experimental){
        s->flags |= COMPAT_F_EXPERIMENT;
        s->exp_calls = 0; s->exp_fails = 0; s->exp_faults = 0;
    } else {
        s->flags &= ~COMPAT_F_EXPERIMENT;
    }

    g_total_patches++;
    compat_event(COMPAT_EV_PATCH, s->name_id, (uint32_t)idx,
                 (uint32_t)action, (uint32_t)(uintptr_t)impl,
                 (uint32_t)(experimental ? 1 : 0), 1,
                 COMPAT_ST_OK,
                 experimental ? "experimental patch" : "patch");
    return 1;
}

// Automatic rollback for experimental patches (E4).
static void compat_rollback_check(void){
    for (int i = 0; i < g_iat_n; i++){
        CompatIatSlot* s = &g_iat[i];
        if (!(s->flags & COMPAT_F_EXPERIMENT)) continue;
        // A fault during the experimental window is an immediate trigger.
        if (s->exp_faults > 0){
            s->flags &= ~COMPAT_F_EXPERIMENT;
            s->flags |= COMPAT_F_ROLLED;
            if (s->prev_impl) *(uint64_t*)s->addr = (uint64_t)(uintptr_t)s->prev_impl;
            s->impl = s->prev_impl;
            g_total_rollbacks++;
            compat_event(COMPAT_EV_ROLLBACK, s->name_id, (uint32_t)i,
                         s->exp_faults, s->exp_calls, 0, 0,
                         COMPAT_ST_ROLLBACK, "fault during experimental patch");
        }
    }
}

extern "C" void compat_set_fault_aggressive(int on){
    g_fault.aggressive = on ? 1 : 0;
}

// =====================================================================
//  Observation (D1)
// =====================================================================
static int compat_observe_enabled(void){ return g_observe_on; }

extern "C" void compat_observe_set(int on){ g_observe_on = on ? 1 : 0; }

// Called by the win32 trampoline around an API call.
extern "C" void compat_note_call(int slot, uint32_t a0, uint32_t a1, uint32_t a2,
                       uint32_t ret, uint8_t status){
    if (slot < 0 || slot >= COMPAT_IAT_MAX) return;
    g_slot_calls[slot]++;
    g_total_calls++;
    if (status == COMPAT_ST_FAIL){ g_slot_fails[slot]++; g_total_fails++; }
    if (status == COMPAT_ST_STUB) g_total_stubs++;

    CompatIatSlot* s = &g_iat[slot];
    if (s->flags & COMPAT_F_EXPERIMENT){
        s->exp_calls++;
        if (status == COMPAT_ST_FAIL) s->exp_fails++;
    }
    if (g_pe_cur >= 0) g_pe[g_pe_cur].last_slot = slot;

    if (!compat_observe_enabled()) return;
    compat_event(COMPAT_EV_API, s->name_id, (uint32_t)slot,
                 a0, a1, a2, ret, status, 0);
}

// =====================================================================
//  Health sampling (E1)
// =====================================================================
static void compat_count_handles(uint32_t* out){
    for (int i = 0; i < 24; i++) out[i] = 0;
    uint32_t counts[24];
    osapi_handle_kind_counts(counts);
    for (int i = 0; i < 24; i++) out[i] = counts[i];
}

extern "C" void compat_health_sample(void){
    uint32_t now = compat_now_ms();
    CompatHealth* h = &g_health;
    h->frame = g_frame;
    h->ts = now;
    h->tick_ms = (g_last_tick_ms && now > g_last_tick_ms) ? (now - g_last_tick_ms) : 0;
    g_last_tick_ms = now;

    compat_count_handles(h->hcount);

    osapi_mem_stats(&h->heap_bytes, &h->heap_allocs, &h->pmm_free);

    h->pe_active = (uint32_t)compat_pe_active();
    h->calls  = g_total_calls;
    h->fails  = g_total_fails;
    h->stubs  = g_total_stubs;
    h->patches = g_total_patches;
    h->leaks  = g_total_leaks;
    h->rollbacks = g_total_rollbacks;

    compat_event(COMPAT_EV_HEALTH, 0xFFFF, 0,
                 h->tick_ms, h->heap_bytes, h->pmm_free, h->pe_active,
                 COMPAT_ST_OK, "health");
}

// =====================================================================
//  Leak sentinel (E3)
// =====================================================================
static void compat_snapshot(void){
    CompatSnapshot* s = &g_snaps[g_snap_head];
    g_snap_head = (g_snap_head + 1) % COMPAT_SNAP_MAX;
    if (g_snap_n < COMPAT_SNAP_MAX) g_snap_n++;
    s->frame = g_frame;
    compat_count_handles(s->hcount);
    s->iat_used = (uint32_t)g_iat_n;
    s->calls = g_total_calls;
    s->fails = g_total_fails;
}

extern "C" void compat_leak_check(void){
    if (g_snap_n < 2) return;
    // Compare the two most recent snapshots.
    int newer = (g_snap_head - 1 + COMPAT_SNAP_MAX) % COMPAT_SNAP_MAX;
    int older = (g_snap_head - 2 + COMPAT_SNAP_MAX) % COMPAT_SNAP_MAX;
    const CompatSnapshot* a = &g_snaps[older];
    const CompatSnapshot* b = &g_snaps[newer];
    if (b->frame <= a->frame) return;

    for (int k = 0; k < 24; k++){
        uint32_t grew = (b->hcount[k] > a->hcount[k]) ? (b->hcount[k] - a->hcount[k]) : 0;
        if (grew < COMPAT_LEAK_MIN_GROWTH) continue;
        // Report only: never kill anything.
        g_total_leaks++;
        char note[COMPAT_EV_STR];
        int j = 0;
        for (int i = 0; i < 10 && note[j]; i++) note[j++] = "kind ";
        const char* kinds[] = { "FREE","HEAP","FILE","DIR","MUTEX","SEM","EVENT",
                                "COND","PROC","THREAD","JOB","TIMER","SOCK","PIPE",
                                "MSGQ","SHM","CFG","WIN","DEV","ARENA","PERF" };
        const char* kn = (k < 21) ? kinds[k] : "?";
        for (int i = 0; kn[i] && j < COMPAT_EV_STR - 1; i++) note[j++] = kn[i];
        note[j] = 0;
        compat_event(COMPAT_EV_LEAK, 0xFFFF, (uint32_t)k,
                     grew, b->hcount[k], a->hcount[k], 0,
                     COMPAT_ST_LEAK, note);
    }
}

// =====================================================================
//  Quality summary (E5)
// =====================================================================
extern "C" void compat_summary(void){
    CompatSummary* s = &g_summary;
    s->ts = compat_now_ms();
    s->pe_active = (uint32_t)compat_pe_active();
    s->calls = g_total_calls;
    s->fails = g_total_fails;
    s->stubs = g_total_stubs;
    s->patches = g_total_patches;
    s->rollbacks = g_total_rollbacks;
    s->fail_rate_pct = g_total_calls ? (g_total_fails * 100u / g_total_calls) : 0;
    s->degrade_pct   = g_total_calls ? (g_total_stubs * 100u / g_total_calls) : 0;

    // Top 3 failing APIs.
    for (int i = 0; i < 3; i++){ s->top[i] = 0xFFFFFFFFu; s->top_fails[i] = 0; }
    for (int i = 0; i < g_iat_n && i < COMPAT_IAT_MAX; i++){
        uint32_t f = g_slot_fails[i];
        if (!f) continue;
        for (int k = 0; k < 3; k++){
            if (f > s->top_fails[k]){
                for (int m = 2; m > k; m--){
                    s->top[m] = s->top[m-1];
                    s->top_fails[m] = s->top_fails[m-1];
                }
                s->top[k] = (uint32_t)i;
                s->top_fails[k] = f;
                break;
            }
        }
    }
    compat_event(COMPAT_EV_SUMMARY, 0xFFFF, 0,
                 s->calls, s->fails, s->stubs, s->patches,
                 COMPAT_ST_OK, "summary");
}

// =====================================================================
//  File protocol
// =====================================================================
static uint8_t g_file_buf[8192];

static int compat_read_file(const char* name, void* buf, int cap){
    if (!g_vol_read) return -1;
    return g_vol_read(name, buf, cap);
}

// Write a file to the data volume.  The volume is sector granular, so the
// reader may see a torn file; the header's crc lets it detect and skip it.
static int compat_write_file(const char* name, const void* data, int len){
    if (!g_vol_write) return -1;
    return g_vol_write(name, (const unsigned char*)data, len);
}

static void compat_flush_events(void){
    if (!g_ring_count) return;
    // Build the payload: header + as many events as fit.
    int n = g_ring_count;
    if (n > 40) n = 40;                 // 40 * 64 = 2560 bytes
    int payload = n * (int)sizeof(CompatEvent);
    if (payload + (int)sizeof(CompatFileHeader) > (int)sizeof(g_file_buf)) return;

    uint8_t* p = g_file_buf;
    CompatFileHeader h;
    h.magic = COMPAT_MAGIC;
    h.version = COMPAT_VERSION;
    h.kind = COMPAT_FILE_EVENTS;
    h.seq = ++g_evt_seq_written;
    h.len = (uint32_t)payload;
    h.crc = crc32_of(&g_ring[0], (uint32_t)payload);
    h.reserved = 0;
    for (uint32_t i = 0; i < sizeof(h); i++) p[i] = ((uint8_t*)&h)[i];
    for (int i = 0; i < n; i++){
        CompatEvent* e = &g_ring[i];
        for (uint32_t k = 0; k < sizeof(*e); k++)
            p[sizeof(h) + i*sizeof(*e) + k] = ((uint8_t*)e)[k];
    }
    compat_write_file("ai_events.bin", g_file_buf,
                      (int)(sizeof(h) + payload));
}

// Parse the command file and apply what we understand.
static void compat_pump_cmds(void){
    int n = compat_read_file("ai_cmd.bin", g_file_buf, (int)sizeof(g_file_buf));
    if (n < (int)sizeof(CompatFileHeader)) return;
    CompatFileHeader* h = (CompatFileHeader*)g_file_buf;
    if (h->magic != COMPAT_MAGIC || h->version != COMPAT_VERSION) return;
    if (h->kind != COMPAT_FILE_CMDS) return;
    if (h->seq <= g_cmd_seq_seen) return;         // already applied
    if (h->len > (uint32_t)(n - (int)sizeof(CompatFileHeader))) return;

    uint8_t* base = g_file_buf + sizeof(CompatFileHeader);
    int count = (int)(h->len / sizeof(CompatCmd));
    for (int i = 0; i < count; i++){
        CompatCmd* c = (CompatCmd*)(base + i * sizeof(CompatCmd));
        switch (c->op){
        case COMPAT_OP_PATCH: {
            // s0 = "dll!fn"
            char dll[24], fn[24];
            int j = 0;
            for (; c->s0[j] && c->s0[j] != '!' && j < 23; j++) dll[j] = c->s0[j];
            dll[j] = 0;
            int k = 0;
            if (c->s0[j] == '!'){
                j++;
                for (; c->s0[j] && k < 23; j++) fn[k++] = c->s0[j];
            }
            fn[k] = 0;
            compat_patch_iat(dll, fn, (int)c->arg0, (int)c->arg1,
                             (c->flags & 1) ? 1 : 0);
            break;
        }
        case COMPAT_OP_FAULT_MODE:
            compat_set_fault_aggressive(c->flags & 1);
            break;
        case COMPAT_OP_OBSERVE:
            g_observe_on = c->arg0 ? 1 : 0;
            break;
        case COMPAT_OP_RESUME:
            compat_pe_resume((int)c->arg0);
            break;
        case COMPAT_OP_EXPERIMENTAL:
            g_evt_seq_written += 0;   // consumed by the next PATCH
            break;
        default:
            break;
        }
    }
    g_cmd_seq_seen = h->seq;
}

// =====================================================================
//  PE execution (D0)
// =====================================================================
extern "C" int compat_pe_active(void){
    int n = 0;
    for (int i = 0; i < COMPAT_PE_MAX; i++) if (g_pe[i].state == PE_STATE_RUNNING) n++;
    return n;
}

extern "C" uint32_t compat_pe_session(void){
    if (g_pe_cur < 0) return 0;
    return g_pe[g_pe_cur].session;
}

// The trampoline the PE entry returns into.  Reaching it means the PE
// finished normally.
extern "C" void compat_pe_entry_trampoline(void);

extern "C" int compat_pe_start(uint64_t entry_rva, uint64_t image_base){
    int slot = -1;
    for (int i = 0; i < COMPAT_PE_MAX; i++)
        if (g_pe[i].state == PE_STATE_IDLE || g_pe[i].state == PE_STATE_EXITED ||
            g_pe[i].state == PE_STATE_ABORTED){ slot = i; break; }
    if (slot < 0) return -1;

    PeContext* p = &g_pe[slot];
    p->state = PE_STATE_RUNNING;
    p->api_calls = 0;
    p->budget = 64;             // yield after this many API calls
    p->faulted = 0;
    p->last_slot = -1;
    p->session = g_pe_next_session++;
    p->retval = 0;
    p->image_base = image_base;
    p->entry_rva = entry_rva;

    // ---- allocate the PE's own stack -------------------------------
    // The PE must NOT run on the kernel stack: a stack overflow inside
    // the PE would then smash the kernel's own frames.  One dedicated
    // stack per session keeps the damage contained and gives the fault
    // handler a known range to recognise a stack overflow.
    if (!p->stack){
        p->stack = (uint64_t*)pmm_alloc_range(COMPAT_STACK_SIZE / 4096u);
        if (!p->stack){
            p->state = PE_STATE_FAILED;
            return -1;
        }
    }

    g_pe_cur = slot;
    return (int)p->session;
}

// The trampoline the PE entry returns into.  Reaching it means the PE
// finished normally.  We are on the PE stack with the entry's return
// value in eax.
extern "C" void compat_pe_entry_trampoline(void){
    int slot = g_pe_cur;
    if (slot >= 0 && slot < COMPAT_PE_MAX){
        g_pe[slot].state = PE_STATE_EXITED;
        g_pe[slot].retval = (uint64_t)(uint32_t)g_pe[slot].pending_ret;
    }
    g_in_pe_exec = 0;
    // Hand control back to the kernel the same way the yield path does.
    compat_longjmp(g_kernel_ctxp, 1);
}

// The cooperative yield point.
//
// The win32 trampoline calls this once the PE has used up its API-call
// budget.  We save the PE's context (setjmp) and longjmp back into the
// kernel checkpoint; the next compat_pe_resume() restores us with another
// longjmp and we return into the PE exactly where we left off.
//
// The context mechanics are the classic setjmp/longjmp ones:
//   compat_setjmp() stores rsp pointing AT the return-address slot and
//   the caller's return address as the resume rip.
//   compat_longjmp() restores rsp, writes the resume rip into that slot
//   and executes `ret`, so the resumed function continues right after its
//   setjmp call with a consistent stack.
extern "C" void compat_pe_yield(void){
    g_in_pe_exec = 0;                       // leaving PE execution
    if (compat_setjmp(&g_pe_ctx) == 0){
        // First time through: park the PE context and return to the kernel.
        // val 1 (not 0) so the resume checkpoint's setjmp returns != 0 and
        // does NOT re-enter the PE-launch branch.
        compat_longjmp(g_kernel_ctxp, 1);
    }
    // Resumed by the kernel: we are back inside the PE.
    g_in_pe_exec = 1;
}

// Advance the PE by at most `budget` API calls.
//
// The PE runs on its own stack.  Every time it calls into a trampoline
// and has used up its call budget, the trampoline calls compat_pe_yield(),
// which parks the PE context and switches back to the kernel.
// compat_pe_resume() then switches the PE back in.
//
// This is the cooperative yield that keeps the frame loop -- and hence
// the AI's view of the world -- alive while the PE runs.
extern "C" int compat_pe_resume(int budget){
    if (budget <= 0) budget = 1;
    if (g_pe_cur < 0) return 0;
    PeContext* p = &g_pe[g_pe_cur];
    if (p->state != PE_STATE_RUNNING) return 0;

    // Top up the call budget for this slice.
    p->budget = (uint32_t)budget;

    // Arm the kernel-side checkpoint.  A longjmp() from the PE (a yield,
    // an exit, or a fault) lands right back here.
    if (compat_setjmp(&g_kernel_ctx) == 0){
        if (!p->started){
            // ---- first entry: build the PE's startup stack ----
            p->started = 1;
            uint64_t* top = (uint64_t*)((uintptr_t)p->stack +
                                       COMPAT_STACK_SIZE - 16);
            uint64_t* sp  = (uint64_t*)((uintptr_t)top & ~(uintptr_t)0xFULL);
            sp -= 1;                       // 16-byte aligned minus 8
            sp[0] = (uint64_t)(uintptr_t)compat_pe_entry_trampoline;
            sp[1] = 0;                     // (unused) fake return address
            // Switch into the PE: we never return from this call; control
            // comes back via a longjmp into the checkpoint above.
            g_in_pe_exec = 1;
            compat_fiber_switch(&g_pe_ctx, (uint64_t)(uintptr_t)sp,
                                p->image_base + p->entry_rva);
        } else {
            // ---- resume the parked PE ----
            g_in_pe_exec = 1;
            compat_longjmp(&g_pe_ctx, 1);
        }
    }
    // We are back: the PE yielded, exited, or faulted.
    g_in_pe_exec = 0;
    return 1;
}

// =====================================================================
//  Textual renderings (managed side + human readers)
// =====================================================================
extern "C" void compat_summary_text(char* out, int cap){
    if (!out || cap < 8) return;
    CompatSummary* s = &g_summary;
    int n = 0;
    #define PUT(str) do { const char* _s = (str); \
        while (*_s && n < cap - 1) out[n++] = *_s++; } while (0)
    #define PUTN(v)  do { char _b[12]; int _i = 0; uint32_t _v = (uint32_t)(v); \
        if (!_v) _b[_i++] = '0'; \
        while (_v && _i < 11) { _b[_i++] = (char)('0' + (_v % 10)); _v /= 10; } \
        while (_i > 0 && n < cap - 1) out[n++] = _b[--_i]; } while (0)
    PUT("pe=");    PUTN(s->pe_active);
    PUT(" calls=");PUTN(s->calls);
    PUT(" fails=");PUTN(s->fails);
    PUT(" stubs=");PUTN(s->stubs);
    PUT(" patches=");PUTN(s->patches);
    PUT(" rollbacks=");PUTN(s->rollbacks);
    PUT(" fail%=");PUTN(s->fail_rate_pct);
    PUT(" degrade%=");PUTN(s->degrade_pct);
    PUT(" top=");
    for (int i = 0; i < 3; i++){
        if (s->top[i] == 0xFFFFFFFFu) continue;
        const char* nm = compat_iat_name((int)s->top[i]);
        PUT(nm ? nm : "?");
        PUT(":");PUTN(s->top_fails[i]); PUT(" ");
    }
    #undef PUT
    #undef PUTN
    out[n] = 0;
}

extern "C" void compat_fault_text(char* out, int cap){
    if (!out || cap < 8) return;
    int n = 0;
    #define PUT(str) do { const char* _s = (str); \
        while (*_s && n < cap - 1) out[n++] = *_s++; } while (0)
    #define PUTN(v)  do { char _b[12]; int _i = 0; uint32_t _v = (uint32_t)(v); \
        if (!_v) _b[_i++] = '0'; \
        while (_v && _i < 11) { _b[_i++] = (char)('0' + (_v % 10)); _v /= 10; } \
        while (_i > 0 && n < cap - 1) out[n++] = _b[--_i]; } while (0)
    if (!g_fault.valid){ PUT("no fault recorded"); }
    else {
        PUT("vec="); PUTN(g_fault.vec);
        PUT(" err="); PUTN(g_fault.err);
        PUT(" rip="); PUTN((uint32_t)(g_fault.rip & 0xFFFFFFFFu));
        PUT(" cr2="); PUTN((uint32_t)(g_fault.cr2 & 0xFFFFFFFFu));
        PUT(" in_pe="); PUTN(g_fault.in_pe);
        PUT(" cause="); PUT(g_fault.cause);
        PUT(" action="); PUT(g_fault.action);
    }
    #undef PUT
    #undef PUTN
    out[n] = 0;
}

// =====================================================================
//  The pump: called once per frame
// =====================================================================
extern "C" int compat_pump(int budget){
    if (budget <= 0) budget = 1;
    int work = 0;

    g_frame++;

    // 1) consume the AI's commands
    compat_pump_cmds();
    work++;

    // 2) advance the PE (bounded)
    if (compat_pe_resume(budget)) work++;

    // 3) experimental-patch watchdog
    compat_rollback_check();

    // 4) health sampling every frame (cheap: a few counters)
    compat_health_sample();

    // 5) leak sentinel every 1000 frames
    if (g_frame >= g_snap_next_frame){
        g_snap_next_frame = g_frame + COMPAT_SNAP_INTERVAL;
        compat_snapshot();
        compat_leak_check();
        work++;
    }

    // 6) quality summary every 3600 frames (~60 s at 60 fps)
    if ((g_frame % 3600u) == 0) compat_summary();

    // 7) publish the event ring to the data volume
    compat_flush_events();

    return work;
}

extern "C" void compat_init(void){
    g_iat_n = 0;
    g_names_n = 0;
    g_ring_head = 0;
    g_ring_count = 0;
    g_event_seq = 0;
    g_frame = 0;
    g_last_tick_ms = 0;
    g_total_calls = g_total_fails = g_total_stubs = 0;
    g_total_patches = g_total_rollbacks = g_total_leaks = 0;
    g_observe_on = 0;
    g_cmd_seq_seen = 0;
    g_evt_seq_written = 0;
    g_dummy_used = 0;
    g_pe_cur = -1;
    for (int i = 0; i < COMPAT_PE_MAX; i++){
        g_pe[i].state = PE_STATE_IDLE;
        g_pe[i].session = 0;
        g_pe[i].last_slot = -1;
    }
    if (!g_dummy_pool){
        g_dummy_pool = (uint8_t*)kmalloc(COMPAT_DUMMY_SLOTS * COMPAT_DUMMY_SIZE);
    }
    // Ask the win32 loader for its builtin resolver, so a patch can rebind
    // an IAT slot to a real implementation instead of a stub.
    compat_set_resolver(compat_resolve_builtin);
}
