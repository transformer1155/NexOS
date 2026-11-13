// =====================================================================
//  osapi.h  -  NexOS.OS API layer 0+1 (error channel, handle table,
//              ABI/capability probes, token + boundary security)
//  ABI version 1.  Every failing icall must set_last_error().
// =====================================================================
#ifndef NEXOS_OSAPI_H
#define NEXOS_OSAPI_H

#include <stdint.h>

#define NEX_ABI_VERSION 1

// ---- capability ids (frozen; bit n = CAP n in caps_mask) -------------
#define CAP_ERR   0
#define CAP_HANDLE 1
#define CAP_PERM  2
#define CAP_PROC  3
#define CAP_THREAD 4
#define CAP_SYNC  5
#define CAP_JOB   6
#define CAP_FS    7
#define CAP_MEM   8
#define CAP_IPC   9
#define CAP_NET   10
#define CAP_DEV   11
#define CAP_CLOCK 12
#define CAP_GFX   13
#define CAP_PKG   14
#define CAP_LOG   15
#define CAP_CFG   16
#define CAP_CLR   17
#define CAP_DBG   18

// ---- error codes (frozen ABI) ----------------------------------------
#define ERR_OK          0
#define ERR_NOENTRY     1
#define ERR_BADARG      2
#define ERR_BADHANDLE   3
#define ERR_PERMDENIED  4
#define ERR_NAMEEXISTS  5
#define ERR_NOTFOUND    6
#define ERR_NOSPACE     7
#define ERR_BUSY        8
#define ERR_BROKEN      9
#define ERR_TIMEOUT     10
#define ERR_TOOBIG      11
#define ERR_NOMEM       12
#define ERR_NOTSUPP     13
#define ERR_IO          14
#define ERR_WOULDBLOCK  15
#define ERR_CROSSDEV    16
#define ERR_NOTDIR      17
#define ERR_ISDIR       18
#define ERR_QUOTA       19
#define ERR_DEADLK      20
#define ERR_STALE       21
#define ERR_CLRFAULT    22

// ---- handle kinds -----------------------------------------------------
#define HK_FREE   0
#define HK_HEAP   1
#define HK_FILE   2
#define HK_DIR    3
#define HK_MUTEX  4
#define HK_SEM    5
#define HK_EVENT  6
#define HK_COND   7
#define HK_PROC   8
#define HK_THREAD 9
#define HK_JOB    10
#define HK_TIMER  11
#define HK_SOCK   12
#define HK_PIPE   13
#define HK_MSGQ   14
#define HK_SHM    15
#define HK_CFG    16
#define HK_WIN    17
#define HK_DEV    18
#define HK_ARENA  19
#define HK_PERF   20

// ---- handle rights ----------------------------------------------------
#define HR_R    1u
#define HR_W    2u
#define HR_X    4u
#define HR_CTRL 8u

typedef uint32_t h_t;   // kind:8 | slot:16 | gen:8

// ---- error channel (kernel side) --------------------------------------
int  osapi_set_last_error(int code);
int  osapi_get_last_error(void);
void osapi_clear_last_error(void);

// ---- handle table ------------------------------------------------------
h_t   osapi_handle_alloc(uint8_t kind, uint16_t slot, uint32_t owner,
                         void* ptr, uint32_t rights);
void* osapi_handle_resolve(h_t h, uint8_t kind, uint32_t owner); // NULL + err on fail
int   osapi_handle_close(h_t h, uint32_t owner);                 // gen++ after close
void  osapi_handle_gen_bump_owned(uint32_t owner);               // boundary change
void  osapi_handle_table_init(void);

// ---- token / boundary --------------------------------------------------
#define FS_MAXVOL    8
#define FS_MAXPREFIX 16
#define TOK_PRIV_R0  1u

typedef struct OsToken {
    uint32_t id;
    uint32_t uid;
    uint32_t parent;
    uint64_t syscall_mask;
    uint32_t caps_mask;
    uint32_t flags;
} OsToken;

typedef struct OsPrefix {
    uint32_t vol;
    char     prefix[64];
    uint32_t mode;   // FSB_RO=1, FSB_RW=2, FSB_NONE=0
} OsPrefix;

typedef struct OsBoundary {
    uint32_t fs_vol_mask;
    OsPrefix prefixes[FS_MAXPREFIX];
    int      n_prefixes;
    uint64_t fs_quota_bytes;
    uint64_t fs_used_bytes;
    uint32_t dev_class_mask;
    uint32_t dev_id_whitelist[16];
    int      n_dev_ids;
    uint32_t net_port_lo, net_port_hi;   // 0/0 = no network
    uint32_t net_cidr[8];                // packed a/b/n triples
    int      n_cidr;                     // 0 = allow any (if port allowed)
    // Syscall classes the boundary permits (bit n == SCL_*).  This is the
    // BOUNDARY's mask, distinct from OsToken::syscall_mask: a child boundary
    // may not grant a syscall class its parent forbids, and
    // osapi_boundary_subset() enforces exactly that.  The token mask is the
    // runtime intersection of the boundary masks along the parent chain.
    uint64_t syscall_mask;
} OsBoundary;

#define FSB_NONE 0u
#define FSB_RO   1u
#define FSB_RW   2u

// syscall classes for syscall_mask (bit n)
#define SCL_FS     0
#define SCL_NET    1
#define SCL_DEV    2
#define SCL_PROC   3
#define SCL_THREAD 4
#define SCL_SYNC   5
#define SCL_MEM    6
#define SCL_IPC    7
#define SCL_GFX    8
#define SCL_MISC   9

OsToken*    osapi_cur_token(void);
void        osapi_set_cur_token(uint32_t id);

// ---- L1.3 ICALL_GUARD -------------------------------------------------
// Every L2-L12 icall starts with this.  Both gates are checked against
// the CURRENT token, so a downgraded child token cannot reach an entry
// its boundary forbids even if it guesses the icall name.  Failure is
// always reported through the error channel (never -1).
#define ICALL_GUARD(cap, cls)                                          \
    do { OsToken* tok__ = osapi_cur_token();                           \
         if (!tok__ || !(tok__->caps_mask & (1u << (cap)))) {          \
             osapi_set_last_error(ERR_PERMDENIED); return 0; }          \
         if (!(tok__->syscall_mask & (1ull << (cls)))) {                \
             osapi_set_last_error(ERR_PERMDENIED); return 0; }          \
    } while (0)
OsToken*    osapi_token_get(uint32_t id);
uint32_t    osapi_token_new(uint32_t uid, uint32_t parent, OsBoundary* b);
OsBoundary* osapi_boundary_of(uint32_t token_id);
int         osapi_boundary_set(uint32_t token_id, const OsBoundary* b); // gen++ on success
int         osapi_boundary_subset(const OsBoundary* child, const OsBoundary* parent);

// Segment-boundary prefix match: /home/user1 must NOT match /home/user10.
int osapi_prefix_hit(const char* path, const char* prefix);
int osapi_fs_path_allowed(uint32_t token_id, const char* path, int mode);
int osapi_dev_allowed(uint32_t token_id, int dev_class, int dev_id);
int osapi_net_port_allowed(uint32_t token_id, int port);
int osapi_net_addr_allowed(uint32_t token_id, uint32_t ip_be);
void osapi_init(void);

// icall handlers (mforms convention: int32_t fn(int32_t* a))
int32_t osapi_ic_set_last_error(int32_t* a);
int32_t osapi_ic_get_last_error(int32_t* a);
int32_t osapi_ic_clear_last_error(int32_t* a);
int32_t osapi_ic_api_version(int32_t* a);
int32_t osapi_ic_caps_mask(int32_t* a);
int32_t osapi_ic_handle_alloc(int32_t* a);
int32_t osapi_ic_handle_close(int32_t* a);
int32_t osapi_ic_uid(int32_t* a);
int32_t osapi_ic_fs_vol_mask(int32_t* a);
int32_t osapi_ic_fs_prefix_count(int32_t* a);
int32_t osapi_ic_fs_prefix_vol(int32_t* a);
int32_t osapi_ic_fs_prefix_mode(int32_t* a);
int32_t osapi_ic_fs_can_path(int32_t* a);
int32_t osapi_ic_dev_allowed(int32_t* a);
int32_t osapi_ic_net_port_allowed(int32_t* a);
int32_t osapi_ic_net_addr_allowed(int32_t* a);
int32_t osapi_ic_syscall_allowed(int32_t* a);
int32_t osapi_ic_self_test(int32_t* a);

// ---- L2: sync primitives / jobs / timers -----------------------------
int32_t osapi_ic_mutex_new(int32_t* a);
int32_t osapi_ic_mutex_lock(int32_t* a);
int32_t osapi_ic_mutex_unlock(int32_t* a);
int32_t osapi_ic_sem_new(int32_t* a);
int32_t osapi_ic_sem_wait(int32_t* a);
int32_t osapi_ic_sem_post(int32_t* a);
int32_t osapi_ic_event_new(int32_t* a);
int32_t osapi_ic_event_wait(int32_t* a);
int32_t osapi_ic_event_set(int32_t* a);
int32_t osapi_ic_event_clear(int32_t* a);
int32_t osapi_ic_cond_new(int32_t* a);
int32_t osapi_ic_cond_wait(int32_t* a);
int32_t osapi_ic_cond_signal(int32_t* a);
int32_t osapi_ic_job_start(int32_t* a);
int32_t osapi_ic_job_status(int32_t* a);
int32_t osapi_ic_job_progress(int32_t* a);
int32_t osapi_ic_job_progress_total(int32_t* a);
int32_t osapi_ic_job_cancel(int32_t* a);
int32_t osapi_ic_job_tick(int32_t* a);
int32_t osapi_ic_timer_new(int32_t* a);
int32_t osapi_ic_timer_poll(int32_t* a);

// ---- L3: VFS / files / directories -----------------------------------
int32_t osapi_ic_vfs_resolve(int32_t* a);
int32_t osapi_ic_file_open(int32_t* a);
int32_t osapi_ic_file_read(int32_t* a);
int32_t osapi_ic_file_write(int32_t* a);
int32_t osapi_ic_file_seek(int32_t* a);
int32_t osapi_ic_file_size(int32_t* a);
int32_t osapi_ic_file_close(int32_t* a);
int32_t osapi_ic_file_flush(int32_t* a);
int32_t osapi_ic_file_rename(int32_t* a);
int32_t osapi_ic_file_unlink(int32_t* a);
int32_t osapi_ic_file_mkdir(int32_t* a);
int32_t osapi_ic_dir_open(int32_t* a);
int32_t osapi_ic_dir_next(int32_t* a);
int32_t osapi_ic_file_lock(int32_t* a);

// ---- L2 processes (cooperative CLR-slot processes) -------------------
int osapi_proc_spawn_c(const char* mex, uint32_t token);   // C API, pid or 0
int32_t osapi_ic_proc_spawn(int32_t* a);
int32_t osapi_ic_proc_wait(int32_t* a);      // -1 running, -2 bad pid, >=0 exit code
int32_t osapi_ic_proc_kill(int32_t* a);
int32_t osapi_ic_proc_exit(int32_t* a);      // called from inside own slot
int32_t osapi_ic_proc_count(int32_t* a);
int32_t osapi_ic_proc_pid_at(int32_t* a);
int32_t osapi_ic_proc_state_at(int32_t* a);
int32_t osapi_ic_proc_tick(int32_t* a);      // reap exited slots
int32_t osapi_ic_proc_self_test(int32_t* a);

// ---- L3 glob ----------------------------------------------------------
int32_t osapi_ic_fs_glob(int32_t* a);        // (dir, pattern) -> newline string

// =====================================================================
//  L12  configuration store  (executed 4th, right after L3)
// ---------------------------------------------------------------------
//  Logical name  "t"  -> canonical path "/data/cfg/t.cfg" (used for the
//  boundary check).  The 64-bit MKFS is a FLAT table with FS_NAME_LEN=20,
//  so the physical entry is the basename "t.cfg" / "t.tmp".  Keeping the
//  full canonical path in OsCfg::path preserves segment-boundary semantics
//  without pretending nested directories exist on disk.
//  Flush is atomic at the FS layer: serialize -> write "<name>.tmp" ->
//  rename tmp to "<name>.cfg" (mkfs.create overwrites, then remove tmp).
// =====================================================================
#define CFG_MAX      8
#define CFG_MAX_KEYS 48
#define CFG_NAME_MAX 16     // <=15 chars: "<name>.cfg" fits FS_NAME_LEN=20
#define CFG_KEY_MAX  32
#define CFG_VAL_MAX  96
#define CFG_FILE_MAX 4096   // serialized file ceiling

typedef struct CfgEntry {
    char key[CFG_KEY_MAX];
    char val[CFG_VAL_MAX];
} CfgEntry;

typedef struct OsCfg {
    uint8_t  used;
    uint8_t  dirty;
    char     name[CFG_NAME_MAX];  // logical name
    char     path[64];            // canonical /data/cfg/<name>.cfg
    CfgEntry e[CFG_MAX_KEYS];
    int      n;
    uint32_t owner;               // token that opened it
} OsCfg;

// Pure C API: no CLR string objects, so kernel self-tests can call these
// outside a managed context (clr_new_str() returns 0 there).
h_t osapi_cfg_open_c(const char* name);
int osapi_cfg_get_c(h_t h, const char* key, char* out, int cap);
int osapi_cfg_set_c(h_t h, const char* key, const char* val);
int osapi_cfg_del_c(h_t h, const char* key);
int osapi_cfg_keys_c(h_t h, int index, char* out, int cap);
int osapi_cfg_flush_c(h_t h);
int osapi_cfg_close_c(h_t h);
int osapi_cfg_count_c(h_t h);   // >=0 entry count, <0 = -ERR_*
int osapi_cfg_dirty_c(h_t h);   // 1/0, <0 = -ERR_*

// Managed-side icalls.  All start with ICALL_GUARD(CAP_CFG, SCL_MISC).
int32_t osapi_ic_cfg_open(int32_t* a);
int32_t osapi_ic_cfg_get(int32_t* a);
int32_t osapi_ic_cfg_set(int32_t* a);
int32_t osapi_ic_cfg_del(int32_t* a);
int32_t osapi_ic_cfg_key_at(int32_t* a);
int32_t osapi_ic_cfg_flush(int32_t* a);
int32_t osapi_ic_cfg_close(int32_t* a);
int32_t osapi_ic_cfg_count(int32_t* a);
int32_t osapi_ic_cfg_dirty(int32_t* a);
int32_t osapi_ic_cfg_self_test(int32_t* a);

// =====================================================================
//  L4  memory  (executed 5th, right after L12)
// ---------------------------------------------------------------------
//  Managed code NEVER receives a pointer.  Every allocation is represented
//  by a packed 32-bit h_t (see the ABI note in the L12 section: MiniCLR is
//  a 32-bit stack machine, so the design doc's `long` handles are `int`
//  here).  Bytes are reachable only through HeapPoke/HeapPeek, which take
//  an offset and bounds-check it.  The real backing store is the kernel's
//  persistent kmalloc heap (16 MiB identity-mapped at 0x500000); it is
//  deliberately NOT the CLR bump heap used by Paint().
//
//  ANTI-DANGLING (two independent generations):
//   1. osapi_handle_close() bumps the global handle-table entry's gen, so
//      a recycled slot cannot make an old h_t resolve.
//   2. Each OsMemBlock keeps its own gen + handle_gen.  Even if the global
//      table ever hands back a matching (kind,slot,gen) triple, comparing
//      handle_gen rejects the stale handle with ERR_STALE.
//  The kernel self-test asserts BOTH: it checks the numeric gen values
//  before/after free and demands ERR_STALE after a same-slot reallocation.
// =====================================================================
#define MEM_MAX_BLOCKS 64
#define MEM_MAX_SHM    16
#define MEM_MAX_MAPS   16
#define MEM_NAME_MAX   24
#define MEM_BLOCK_CAP  (1u << 20)   // 1 MiB ceiling per allocation

// mapping "what" codes -> dev_class used by osapi_dev_allowed()
#define MM_WHAT_FB     1
#define MM_WHAT_MMIO   2
#define MM_DEV_FB      0
#define MM_DEV_MMIO    2

typedef struct OsMemBlock {
    uint8_t  used;
    uint8_t  kind;         // HK_HEAP / HK_ARENA / HK_SHM
    uint8_t  gen;          // per-slot generation (L4's own stale guard)
    uint8_t  handle_gen;   // gen packed into the live handle
    uint32_t owner;
    uint32_t size;         // capacity in bytes
    uint32_t used_bytes;   // arena bump cursor; heap/shm == size
    void*    base;         // owned kmalloc block for HEAP/ARENA; 0 for SHM alias
    uint16_t backing;      // SHM: index into g_shm[]; others: 0
    char     name[MEM_NAME_MAX];
} OsMemBlock;

typedef struct OsShmSeg {
    uint8_t  used;
    char     name[MEM_NAME_MAX];
    void*    base;
    uint32_t size;
    uint32_t refs;         // number of live handles (create + open)
} OsShmSeg;

typedef struct OsMemMap {
    uint8_t  used;
    uint8_t  what;
    uint16_t pad;
    uint32_t addr;
    uint32_t len;
    uint32_t owner;
} OsMemMap;

// ---- Pure C API: no CLR objects, callable from kernel self-tests ------
h_t    osapi_heap_alloc_c(uint32_t sz);
int    osapi_heap_free_c(h_t h);
h_t    osapi_heap_realloc_c(h_t h, uint32_t sz);
int    osapi_heap_size_c(h_t h);                  // <0 = -ERR_*
int    osapi_heap_write_c(h_t h, uint32_t off, const unsigned char* p, int n);
int    osapi_heap_read_c(h_t h, uint32_t off, unsigned char* p, int n);
void*  osapi_mem_ptr_c(h_t h);                    // ring-0 ONLY
int    osapi_mem_slot_gen_c(int slot);            // self-test: gen++ evidence
int    osapi_mem_live_c(void);                    // live block count

h_t    osapi_arena_new_c(uint32_t cap);
int    osapi_arena_alloc_c(h_t h, uint32_t sz);   // >=0 byte offset, <0 err
int    osapi_arena_mark_c(h_t h);
int    osapi_arena_release_c(h_t h, int mark);
int    osapi_arena_used_c(h_t h);
int    osapi_arena_free_c(h_t h);

h_t    osapi_shm_create_c(const char* name, uint32_t sz);
h_t    osapi_shm_open_c(const char* name);
int    osapi_shm_size_c(h_t h);
int    osapi_shm_close_c(h_t h);

int    osapi_mmap_c(int what, uint32_t addr, uint32_t len);  // >0 map id
int    osapi_munmap_c(int id);
int    osapi_mmap_count_c(void);

// ---- Managed-side icalls: all start ICALL_GUARD(CAP_MEM, SCL_MEM) -----
int32_t osapi_ic_mem_heap_alloc(int32_t* a);
int32_t osapi_ic_mem_heap_free(int32_t* a);
int32_t osapi_ic_mem_heap_realloc(int32_t* a);
int32_t osapi_ic_mem_heap_size(int32_t* a);
int32_t osapi_ic_mem_heap_poke(int32_t* a);
int32_t osapi_ic_mem_heap_peek(int32_t* a);
int32_t osapi_ic_mem_arena_new(int32_t* a);
int32_t osapi_ic_mem_arena_alloc(int32_t* a);
int32_t osapi_ic_mem_arena_mark(int32_t* a);
int32_t osapi_ic_mem_arena_release(int32_t* a);
int32_t osapi_ic_mem_arena_used(int32_t* a);
int32_t osapi_ic_mem_arena_free(int32_t* a);
int32_t osapi_ic_mem_shm_create(int32_t* a);
int32_t osapi_ic_mem_shm_open(int32_t* a);
int32_t osapi_ic_mem_shm_size(int32_t* a);
int32_t osapi_ic_mem_shm_close(int32_t* a);
int32_t osapi_ic_mem_mmap(int32_t* a);
int32_t osapi_ic_mem_unmap(int32_t* a);
int32_t osapi_ic_mem_mappings(int32_t* a);
int32_t osapi_ic_mem_live(int32_t* a);
int32_t osapi_ic_mem_self_test(int32_t* a);

// ---- AI-assisted compatibility layer (D-class) ----------------------
// The kernel runs no AI.  An external agent watches the PE through the
// MKFS data volume (ai_events.bin / ai_cmd.bin) and feeds back decisions.
// These icalls are the MANAGED-side (C#) entry points; the external agent
// uses the same functions through the file protocol.
// The action codes (COMPAT_ACT_*) live in nexos_compat.h.

// a: dll-name, fn-name, action, sel, flags
//   flags bit0 = experimental (auto-rollback if it faults, see E4)
// returns 1 on success, 0 on failure (last_error is set).
int32_t osapi_ic_compat_patch(int32_t* a);
// a: unused -> returns the number of registered IAT slots.
int32_t osapi_ic_compat_status(int32_t* a);
// a: index, buf, cap -> textual description of one IAT slot.
int32_t osapi_ic_compat_describe(int32_t* a);
// a: on/off -> enable or disable API observation.
int32_t osapi_ic_compat_observe(int32_t* a);
// a: buf, cap -> fills the latest quality summary as text.
int32_t osapi_ic_compat_summary(int32_t* a);
// a: buf, cap -> fills the last fault report as text.
int32_t osapi_ic_compat_fault(int32_t* a);

extern "C" {
// observability accessors used by the compat layer's health sampler.
void osapi_handle_kind_counts(uint32_t* out);
void osapi_mem_stats(uint32_t* heap_bytes, uint32_t* heap_allocs,
                      uint32_t* pmm_free);
void osapi_mem_stats_attach(void (*fn)(uint32_t*, uint32_t*, uint32_t*));
}

// shared time base (monotonic ms, same rdtsc calibration as mforms)
uint32_t osapi_now_ms(void);

#endif // NEXOS_OSAPI_H
