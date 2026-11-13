// =====================================================================
//  osapi.cpp  -  NexOS.OS API layer 0+1 implementation
//  Freestanding 64-bit kernel TU.  No stdlib.  All failing icalls
//  set_last_error(); no bare-int handles escape this TU untracked.
// =====================================================================
#include "osapi.h"
#include "clr.h"

// ---- serial (own copy; avoid pulling kernel64.cpp deps) ---------------
static inline void os_outb(uint16_t p, uint8_t v){
    __asm__ __volatile__("outb %0,%1" :: "a"(v), "Nd"(p));
}
static void oser(const char* s){ while (*s) os_outb(0x3F8, (uint8_t)*s++); }
static void oser_dec(int v){
    char b[12]; int i = 0;
    if (v == 0) b[i++] = '0';
    else { int t = v; if (v < 0) { b[i++] = '-'; t = -t; }
           while (t) { b[i++] = (char)('0' + t % 10); t /= 10; } }
    for (int j = i - 1; j >= 0; j--) os_outb(0x3F8, (uint8_t)b[j]);
}
static int ostr_eq(const char* a, const char* b){
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}
static void ostr_cpy(char* d, const char* s, int cap){
    int i = 0;
    while (s && s[i] && i < cap - 1) { d[i] = s[i]; i++; }
    d[i] = 0;
}

// =====================================================================
//  L0.1  error channel
// =====================================================================
#define MAX_TOKENS_OS 16
static int g_last_error[MAX_TOKENS_OS] = {0};   // per-token error slot
static uint32_t g_active_token = 1;             // 1 = init/shell default

int osapi_set_last_error(int code){
    int t = (int)g_active_token;
    if (t < 0 || t >= MAX_TOKENS_OS) t = 0;
    int old = g_last_error[t];
    g_last_error[t] = code;
    (void)old;
    return code;
}
int osapi_get_last_error(void){
    int t = (int)g_active_token;
    if (t < 0 || t >= MAX_TOKENS_OS) t = 0;
    return g_last_error[t];
}
void osapi_clear_last_error(void){
    int t = (int)g_active_token;
    if (t < 0 || t >= MAX_TOKENS_OS) t = 0;
    g_last_error[t] = ERR_OK;
}

// =====================================================================
//  L0.2  global handle table
// =====================================================================
#define HK_MAX 512
struct OsHandleEntry {
    uint8_t  kind;
    uint16_t slot;
    uint8_t  gen;
    uint32_t owner;
    void*    ptr;
    uint32_t rights;
};
static OsHandleEntry g_htab[HK_MAX];
static int g_hcount = 0;

static h_t hpack(uint8_t kind, uint16_t slot, uint8_t gen){
    return ((uint32_t)kind) | ((uint32_t)slot << 8) | ((uint32_t)gen << 24);
}
static uint8_t hkind(h_t h){ return (uint8_t)(h & 0xFF); }
static uint16_t hslot(h_t h){ return (uint16_t)((h >> 8) & 0xFFFF); }
static uint8_t hgen(h_t h){ return (uint8_t)((h >> 24) & 0xFF); }

void osapi_handle_table_init(void){
    for (int i = 0; i < HK_MAX; i++){
        g_htab[i].kind = HK_FREE; g_htab[i].slot = 0; g_htab[i].gen = 1;
        g_htab[i].owner = 0; g_htab[i].ptr = 0; g_htab[i].rights = 0;
    }
    g_hcount = 0;
}

h_t osapi_handle_alloc(uint8_t kind, uint16_t slot, uint32_t owner,
                       void* ptr, uint32_t rights){
    for (int i = 0; i < HK_MAX; i++){
        if (g_htab[i].kind != HK_FREE) continue;
        g_htab[i].kind   = kind;
        g_htab[i].slot   = slot;
        // keep gen (reuses generation so old handles from a recycled slot
        // still mismatch by gen when the slot was closed earlier)
        g_htab[i].owner  = owner;
        g_htab[i].ptr    = ptr;
        g_htab[i].rights = rights;
        g_hcount++;
        return hpack(kind, slot, g_htab[i].gen);
    }
    osapi_set_last_error(ERR_NOSPACE);
    return 0;
}

void* osapi_handle_resolve(h_t h, uint8_t kind, uint32_t owner){
    uint8_t  k = hkind(h);
    uint16_t s = hslot(h);
    uint8_t  g = hgen(h);
    if (k == HK_FREE || h == 0) { osapi_set_last_error(ERR_BADHANDLE); return 0; }
    if (kind != 0xFF && k != kind) { osapi_set_last_error(ERR_BADHANDLE); return 0; }
    for (int i = 0; i < HK_MAX; i++){
        if (g_htab[i].kind == k && g_htab[i].slot == s){
            if (g_htab[i].gen != g) { osapi_set_last_error(ERR_STALE); return 0; }
            if (owner != 0 && g_htab[i].owner != owner && g_htab[i].owner != 0){
                osapi_set_last_error(ERR_BADHANDLE); return 0;
            }
            return g_htab[i].ptr;
        }
    }
    osapi_set_last_error(ERR_BADHANDLE);
    return 0;
}

// ---- observability accessors (E1: the AI compat layer samples these) ---
// The handle table is file-static, so the compat layer cannot read it
// directly.  These two accessors expose exactly what the health sampler
// needs and nothing more.
//
// osapi_handle_kind_counts: live handle count per HK_* kind.
//   The caller passes an array of at least 24 uint32 slots; kinds above 23
//   are folded into the last bucket so nothing is silently lost.
extern "C" void osapi_handle_kind_counts(uint32_t* out){
    if (!out) return;
    for (int i = 0; i < 24; i++) out[i] = 0;
    for (int i = 0; i < HK_MAX; i++){
        uint8_t k = g_htab[i].kind;
        if (k == HK_FREE) continue;
        if (k < 24) out[k]++;
        else         out[23]++;
    }
}

// osapi_mem_stats: kernel heap and physical page watermarks.
//   The heap counters live in kernel64.cpp, which installs a provider here
//   at boot; until then we report zeros rather than guessing.
static void (*g_mem_stats_provider)(uint32_t*, uint32_t*, uint32_t*) = 0;
extern "C" void osapi_mem_stats_attach(void (*fn)(uint32_t*, uint32_t*, uint32_t*)){
    g_mem_stats_provider = fn;
}
extern "C" void osapi_mem_stats(uint32_t* heap_bytes, uint32_t* heap_allocs,
                                uint32_t* pmm_free){
    if (heap_bytes)  *heap_bytes  = 0;
    if (heap_allocs) *heap_allocs = 0;
    if (pmm_free)    *pmm_free    = 0;
    if (g_mem_stats_provider)
        g_mem_stats_provider(heap_bytes, heap_allocs, pmm_free);
}

int osapi_handle_close(h_t h, uint32_t owner){
    uint8_t  k = hkind(h);
    uint16_t s = hslot(h);
    uint8_t  g = hgen(h);
    if (h == 0 || k == HK_FREE){ osapi_set_last_error(ERR_BADHANDLE); return 0; }
    for (int i = 0; i < HK_MAX; i++){
        if (g_htab[i].kind == k && g_htab[i].slot == s){
            if (g_htab[i].gen != g){ osapi_set_last_error(ERR_STALE); return 0; }
            if (owner != 0 && g_htab[i].owner != owner && g_htab[i].owner != 0){
                osapi_set_last_error(ERR_BADHANDLE); return 0;
            }
            // invalidate: free + generation bump so stale handles never resolve
            g_htab[i].gen++;
            if (g_htab[i].gen == 0) g_htab[i].gen = 1;
            g_htab[i].kind = HK_FREE;
            g_htab[i].ptr = 0;
            g_hcount--;
            return 1;
        }
    }
    osapi_set_last_error(ERR_BADHANDLE);
    return 0;
}

void osapi_handle_gen_bump_owned(uint32_t owner){
    for (int i = 0; i < HK_MAX; i++){
        if (g_htab[i].kind != HK_FREE && g_htab[i].owner == owner){
            g_htab[i].gen++;
            if (g_htab[i].gen == 0) g_htab[i].gen = 1;
        }
    }
}

// =====================================================================
//  L1  tokens and boundaries
// =====================================================================
static OsToken    g_tokens[MAX_TOKENS_OS];
static OsBoundary g_bounds[MAX_TOKENS_OS];
static int g_ntokens = 0;

void osapi_set_cur_token(uint32_t id){ g_active_token = id; }

OsToken* osapi_cur_token(void){
    return osapi_token_get(g_active_token);
}

OsToken* osapi_token_get(uint32_t id){
    for (int i = 0; i < g_ntokens; i++)
        if (g_tokens[i].id == id) return &g_tokens[i];
    return 0;
}

uint32_t osapi_token_new(uint32_t uid, uint32_t parent, OsBoundary* b){
    if (g_ntokens >= MAX_TOKENS_OS){ osapi_set_last_error(ERR_NOSPACE); return 0; }
    OsToken* t = &g_tokens[g_ntokens];
    OsBoundary* bd = &g_bounds[g_ntokens];
    int idx = g_ntokens;
    g_ntokens++;
    t->id = (uint32_t)(idx + 1);      // token ids start at 1 (0 = kernel)
    t->uid = uid;
    t->parent = parent;
    t->caps_mask = 0xFFFFFFFFu;       // caps restricted by boundary enforcement, not here
    t->syscall_mask = 0xFFFFFFFFull;
    t->flags = parent == 0 ? TOK_PRIV_R0 : 0;
    if (b) *bd = *b;
    else {
        // default boundary: full access (kernel/init token)
        for (int i = 0; i < FS_MAXPREFIX; i++) bd->prefixes[i].mode = FSB_NONE;
        bd->n_prefixes = 0;
        bd->fs_vol_mask = 0xFFFFFFFFu;
        bd->fs_quota_bytes = 0; bd->fs_used_bytes = 0;
        bd->dev_class_mask = 0xFFFFFFFFu;
        bd->n_dev_ids = 0;
        bd->net_port_lo = 0; bd->net_port_hi = 0xFFFF;
        bd->n_cidr = 0;
        bd->syscall_mask = 0xFFFFFFFFull;   // default boundary: all syscall classes
    }
    osapi_set_last_error(ERR_OK);
    return t->id;
}

OsBoundary* osapi_boundary_of(uint32_t token_id){
    for (int i = 0; i < g_ntokens; i++)
        if (g_tokens[i].id == token_id) return &g_bounds[i];
    return 0;
}

// ring-0 entry: replace boundary + invalidate all handles owned by the token
int osapi_boundary_set(uint32_t token_id, const OsBoundary* b){
    OsBoundary* cur = osapi_boundary_of(token_id);
    if (!cur){ osapi_set_last_error(ERR_NOENTRY); return 0; }
    *cur = *b;
    osapi_handle_gen_bump_owned(token_id);   // hard guarantee: stale handles die here
    osapi_set_last_error(ERR_OK);
    return 1;
}

// Does a parent prefix mode grant the child's requested mode?
// FSB_NONE grants nothing; FSB_RO grants read; FSB_RW grants read+write.
static int mode_grants(uint32_t parent_mode, uint32_t child_mode){
    if (child_mode == FSB_NONE) return 1;
    if (child_mode == FSB_RO)   return parent_mode == FSB_RO || parent_mode == FSB_RW;
    if (child_mode == FSB_RW)   return parent_mode == FSB_RW;
    return 0;
}

// child must be a strict subset of parent (monotonic downgrade)
int osapi_boundary_subset(const OsBoundary* child, const OsBoundary* parent){
    if ((child->fs_vol_mask & ~parent->fs_vol_mask)) return 0;
    for (int i = 0; i < child->n_prefixes; i++){
        int ok = 0;
        for (int j = 0; j < parent->n_prefixes; j++){
            if (child->prefixes[i].vol != parent->prefixes[j].vol) continue;
            // child prefix must live INSIDE the parent prefix:
            // prefix_hit(path=child_prefix, prefix=parent_prefix)
            if (!osapi_prefix_hit(child->prefixes[i].prefix,
                                  parent->prefixes[j].prefix)) continue;
            if (!mode_grants(parent->prefixes[j].mode,
                             child->prefixes[i].mode)) continue;
            ok = 1; break;
        }
        if (!ok) return 0;
    }
    if (parent->fs_quota_bytes != 0 &&
        child->fs_quota_bytes > parent->fs_quota_bytes) return 0;
    if ((child->dev_class_mask & ~parent->dev_class_mask)) return 0;
    if (child->net_port_lo < parent->net_port_lo) return 0;
    if (child->net_port_hi > parent->net_port_hi) return 0;
    if ((child->syscall_mask & ~parent->syscall_mask)) return 0;
    return 1;
}

// ---------------------------------------------------------------------
//  Segment-boundary prefix match (the ONLY sanctioned implementation).
//  /home/user1 vs /home/user10 must NOT match; /home/user1/x must;
//  /home/user1 alone (exact) matches.
// ---------------------------------------------------------------------
int osapi_prefix_hit(const char* path, const char* prefix){
    if (!path || !prefix) return 0;
    int n = 0;
    while (prefix[n]) {
        if (path[n] != prefix[n]) return 0;
        n++;
    }
    char c = path[n];
    return (c == 0 || c == '/');
}

int osapi_fs_path_allowed(uint32_t token_id, const char* path, int mode){
    OsBoundary* b = osapi_boundary_of(token_id);
    if (!b){ osapi_set_last_error(ERR_NOENTRY); return 0; }
    // no prefixes configured = full access (kernel/init default)
    if (b->n_prefixes == 0) { osapi_set_last_error(ERR_OK); return 1; }
    for (int i = 0; i < b->n_prefixes; i++){
        if (!osapi_prefix_hit(path, b->prefixes[i].prefix)) continue;
        if (mode_grants(b->prefixes[i].mode, (uint32_t)mode)){
            osapi_set_last_error(ERR_OK);
            return 1;
        }
        osapi_set_last_error(ERR_PERMDENIED);
        return 0;
    }
    osapi_set_last_error(ERR_PERMDENIED);
    return 0;
}

int osapi_dev_allowed(uint32_t token_id, int dev_class, int dev_id){
    OsBoundary* b = osapi_boundary_of(token_id);
    if (!b){ osapi_set_last_error(ERR_NOENTRY); return 0; }
    if (dev_class < 0 || dev_class > 31 ||
        !(b->dev_class_mask & (1u << dev_class))){
        osapi_set_last_error(ERR_PERMDENIED); return 0;
    }
    if (b->n_dev_ids > 0){
        int found = 0;
        for (int i = 0; i < b->n_dev_ids; i++)
            if ((int)b->dev_id_whitelist[i] == dev_id){ found = 1; break; }
        if (!found){ osapi_set_last_error(ERR_PERMDENIED); return 0; }
    }
    osapi_set_last_error(ERR_OK);
    return 1;
}

int osapi_net_port_allowed(uint32_t token_id, int port){
    OsBoundary* b = osapi_boundary_of(token_id);
    if (!b){ osapi_set_last_error(ERR_NOENTRY); return 0; }
    if (port < 0 || (uint32_t)port < b->net_port_lo ||
        (uint32_t)port > b->net_port_hi){
        osapi_set_last_error(ERR_PERMDENIED); return 0;
    }
    osapi_set_last_error(ERR_OK);
    return 1;
}

int osapi_net_addr_allowed(uint32_t token_id, uint32_t ip_be){
    OsBoundary* b = osapi_boundary_of(token_id);
    if (!b){ osapi_set_last_error(ERR_NOENTRY); return 0; }
    if (b->n_cidr == 0){ osapi_set_last_error(ERR_OK); return 1; }
    // packed CIDR: (a<<24) | (b<<16) | n, where a/b are the first two
    // address bytes (big-endian high half) and n the prefix length in bits.
    // The compare masks the top n bits of both ip and the 16-bit prefix
    // (extended with zeros) -- exact for n <= 16, which covers LAN use.
    for (int i = 0; i < b->n_cidr; i++){
        uint32_t v  = b->net_cidr[i];
        uint32_t nn = v & 0xFFFF;
        if (nn == 0 || nn > 32) continue;
        uint32_t pre16 = (v >> 16) & 0xFFFF;         // top 2 bytes of network
        uint32_t mask  = nn == 0 ? 0 : (0xFFFFFFFFu << (32 - nn));
        uint32_t ipnet = ip_be & mask;
        uint32_t prnet = ((pre16 << 16) & mask);     // zero-extended prefix
        // For nn <= 16 the zero-extended prefix compare is exact.
        if (ipnet == prnet){ osapi_set_last_error(ERR_OK); return 1; }
    }
    osapi_set_last_error(ERR_PERMDENIED);
    return 0;
}

void osapi_init(void){
    osapi_handle_table_init();
    g_ntokens = 0;
    g_active_token = 1;
    // token 1 = kernel/init: full boundary (no prefixes = full access)
    osapi_token_new(0, 0, 0);
    g_tokens[0].flags |= TOK_PRIV_R0;
    oser("[OSAPI] layer0+1 armed: abi=v"); oser_dec(NEX_ABI_VERSION);
    oser(" htab="); oser_dec(HK_MAX); oser("\n");
}

// =====================================================================
//  icall handlers (mforms convention)
// =====================================================================
int32_t osapi_ic_set_last_error(int32_t* a){
    osapi_set_last_error((int)a[0]);
    return 1;
}
int32_t osapi_ic_get_last_error(int32_t*){ return (int32_t)osapi_get_last_error(); }
int32_t osapi_ic_clear_last_error(int32_t*){ osapi_clear_last_error(); return 1; }
int32_t osapi_ic_api_version(int32_t*){ return NEX_ABI_VERSION; }
int32_t osapi_ic_caps_mask(int32_t*){
    // Advertise only the layers that are actually wired in this build.
    // L0/L1, L2 (sync/job/timer/process), L3 (VFS/file/dir/glob), L12
    // (cfg) and L4 (heap/arena/shm/mmap) are live; the rest stay dark
    // until their layer lands.
    uint32_t m = (1u << CAP_ERR)   | (1u << CAP_HANDLE) | (1u << CAP_PERM)  |
                 (1u << CAP_PROC)  | (1u << CAP_SYNC)   | (1u << CAP_JOB)    |
                 (1u << CAP_CLOCK) | (1u << CAP_FS)     | (1u << CAP_CFG)    |
                 (1u << CAP_MEM);
    return (int32_t)m;
}

int32_t osapi_ic_handle_alloc(int32_t* a){
    // a: kind, slot, rights  -> h (0 fail)
    h_t h = osapi_handle_alloc((uint8_t)a[0], (uint16_t)a[1],
                               g_active_token, 0, (uint32_t)a[2]);
    return (int32_t)h;
}
int32_t osapi_ic_handle_close(int32_t* a){
    return (int32_t)osapi_handle_close((h_t)a[0], g_active_token);
}

// ---- Perm (read-only facade) ----------------------------------------
int32_t osapi_ic_uid(int32_t*){
    OsToken* t = osapi_cur_token();
    return t ? (int32_t)t->uid : -1;
}
int32_t osapi_ic_fs_vol_mask(int32_t*){
    OsBoundary* b = osapi_boundary_of(g_active_token);
    return b ? (int32_t)b->fs_vol_mask : 0;
}
int32_t osapi_ic_fs_prefix_count(int32_t*){
    OsBoundary* b = osapi_boundary_of(g_active_token);
    return b ? (int32_t)b->n_prefixes : -1;
}
int32_t osapi_ic_fs_prefix_vol(int32_t* a){
    OsBoundary* b = osapi_boundary_of(g_active_token);
    if (!b || a[0] < 0 || a[0] >= b->n_prefixes) { osapi_set_last_error(ERR_BADARG); return 0; }
    return (int32_t)b->prefixes[a[0]].vol;
}
int32_t osapi_ic_fs_prefix_mode(int32_t* a){
    OsBoundary* b = osapi_boundary_of(g_active_token);
    if (!b || a[0] < 0 || a[0] >= b->n_prefixes) { osapi_set_last_error(ERR_BADARG); return 0; }
    return (int32_t)b->prefixes[a[0]].mode;
}
int32_t osapi_ic_fs_can_path(int32_t* a){
    const char* p = clr_str(a[0]);
    if (!p){ osapi_set_last_error(ERR_BADARG); return 0; }
    return (int32_t)osapi_fs_path_allowed(g_active_token, p, (int)a[1]);
}
int32_t osapi_ic_dev_allowed(int32_t* a){
    return (int32_t)osapi_dev_allowed(g_active_token, (int)a[0], (int)a[1]);
}
int32_t osapi_ic_net_port_allowed(int32_t* a){
    return (int32_t)osapi_net_port_allowed(g_active_token, (int)a[0]);
}
int32_t osapi_ic_net_addr_allowed(int32_t* a){
    return (int32_t)osapi_net_addr_allowed(g_active_token, (uint32_t)a[0]);
}
int32_t osapi_ic_syscall_allowed(int32_t* a){
    OsToken* t = osapi_cur_token();
    if (!t){ osapi_set_last_error(ERR_NOENTRY); return 0; }
    if (a[0] < 0 || a[0] > 63){ osapi_set_last_error(ERR_BADARG); return 0; }
    return (t->syscall_mask & (1ull << a[0])) ? 1 : 0;
}

// =====================================================================
//  self-test (serial): handle lifecycle, stale gen, prefix boundary,
//  boundary subset, downgrade rule.  Returns number of failures.
// =====================================================================
int32_t osapi_ic_self_test(int32_t*){
    int fails = 0;
    oser("[OSAPI] self-test begin\n");

    // 1) error channel
    osapi_clear_last_error();
    osapi_set_last_error(ERR_BUSY);
    if (osapi_get_last_error() != ERR_BUSY) { fails++; oser("  ERR channel broken\n"); }

    // 2) handle alloc/resolve/close + double close
    h_t h = osapi_handle_alloc(HK_HEAP, 7, g_active_token, (void*)0x1234, HR_R);
    if (!h) fails++;
    void* p = osapi_handle_resolve(h, HK_HEAP, g_active_token);
    if (p != (void*)0x1234) { fails++; oser("  handle resolve broken\n"); }
    if (!osapi_handle_close(h, g_active_token)) fails++;
    if (osapi_handle_close(h, g_active_token)) { fails++; oser("  double close accepted\n"); }
    if (osapi_get_last_error() != ERR_BADHANDLE) { fails++; oser("  double-close err != BadHandle\n"); }

    // 3) stale generation: alloc, bump gen externally, resolve must fail
    h_t h2 = osapi_handle_alloc(HK_HEAP, 8, g_active_token, 0, HR_R);
    osapi_handle_gen_bump_owned(g_active_token);
    if (osapi_handle_resolve(h2, HK_HEAP, g_active_token)) {
        fails++; oser("  stale handle resolved!\n");
    } else if (osapi_get_last_error() != ERR_STALE) {
        fails++; oser("  stale err != Stale\n");
    }
    osapi_handle_close(h2, g_active_token);

    // 4) prefix segment boundary
    if (osapi_prefix_hit("/home/user10/x", "/home/user1")) { fails++; oser("  user10 leaked into user1\n"); }
    if (!osapi_prefix_hit("/home/user1/x", "/home/user1"))  { fails++; oser("  user1/x denied\n"); }
    if (!osapi_prefix_hit("/home/user1", "/home/user1"))    { fails++; oser("  exact prefix denied\n"); }

    // 5) boundary subset: child claiming parent-foreign volume must fail
    OsBoundary parent; OsBoundary child;
    for (int i = 0; i < FS_MAXPREFIX; i++){
        parent.prefixes[i].mode = FSB_NONE; child.prefixes[i].mode = FSB_NONE;
        parent.prefixes[i].prefix[0] = 0;   child.prefixes[i].prefix[0] = 0;
    }
    parent.n_prefixes = 2; child.n_prefixes = 1;
    parent.fs_vol_mask = 0x1; child.fs_vol_mask = 0x1;
    parent.prefixes[0].vol = 1; ostr_cpy(parent.prefixes[0].prefix, "/data", 64);
    parent.prefixes[0].mode = FSB_RW;
    parent.prefixes[1].vol = 1; ostr_cpy(parent.prefixes[1].prefix, "/home", 64);
    parent.prefixes[1].mode = FSB_RO;
    child.prefixes[0].vol = 1; ostr_cpy(child.prefixes[0].prefix, "/data/calc", 64);
    child.prefixes[0].mode = FSB_RO;
    parent.fs_quota_bytes = 0; child.fs_quota_bytes = 0;
    parent.dev_class_mask = 0xFFFFFFFFu; child.dev_class_mask = 0xFFFFFFFFu;
    parent.net_port_lo = 0; parent.net_port_hi = 0xFFFF;
    child.net_port_lo = 0; child.net_port_hi = 0xFFFF;
    parent.syscall_mask = child.syscall_mask = 0xFFFFFFFFull;
    if (!osapi_boundary_subset(&child, &parent)) { fails++; oser("  valid subset rejected\n"); }
    // 5b) child prefix OUTSIDE parent must fail
    ostr_cpy(child.prefixes[0].prefix, "/etc", 64);
    if (osapi_boundary_subset(&child, &parent)) { fails++; oser("  outside-prefix accepted!\n"); }
    // 5c) prefix inside but with MORE rights must fail
    ostr_cpy(child.prefixes[0].prefix, "/data/calc", 64);
    child.prefixes[0].mode = FSB_RW;   // parent /data has RW? yes -> make parent stricter
    parent.prefixes[0].mode = FSB_RO;
    if (osapi_boundary_subset(&child, &parent)) { fails++; oser("  privilege escalation accepted!\n"); }

    // 6) boundary set invalidates handles (hard requirement)
    h_t h3 = osapi_handle_alloc(HK_HEAP, 9, g_active_token, 0, HR_R);
    osapi_boundary_set(g_active_token, &parent);
    if (osapi_handle_resolve(h3, HK_HEAP, g_active_token)) {
        fails++; oser("  boundary change did NOT invalidate handles\n");
    }
    osapi_handle_close(h3, g_active_token);
    // restore full access (clean boundary: no prefixes) so later layers
    // (L2/L3 file self-test and the managed shell) run unrestricted.
    {
        OsBoundary clean;
        clean.n_prefixes = 0;
        clean.fs_vol_mask = 0xFFFFFFFFu;
        clean.fs_quota_bytes = 0; clean.fs_used_bytes = 0;
        clean.dev_class_mask = 0xFFFFFFFFu;
        clean.n_dev_ids = 0;
        clean.net_port_lo = 0; clean.net_port_hi = 0xFFFF;
        clean.n_cidr = 0;
        osapi_boundary_set(g_active_token, &clean);
    }

    oser("[OSAPI] self-test done, fails=");
    oser_dec(fails);
    oser("\n");

    return fails;
}

// =====================================================================
//  L2  time base
// =====================================================================
uint32_t osapi_now_ms(void){
    uint32_t t;
    __asm__ volatile("rdtsc" : "=a"(t) :: "edx");
    return (uint32_t)(t >> 10);   // same >>10 calibration as mforms h_ticks
}

static void spin_wait_ms(int ms){
    if (ms <= 0) return;
    uint32_t start = osapi_now_ms();
    while ((int)(osapi_now_ms() - start) < ms) { }
}

// =====================================================================
//  L2  sync primitives (cooperative kernel: waits are bounded spins;
//  ownership tracking gives deadlock detection via Err_Deadlk)
// =====================================================================
struct OsSyncObj {
    uint8_t  used;
    uint8_t  type;       // HK_MUTEX/HK_SEM/HK_EVENT/HK_COND
    uint32_t owner;      // mutex owner token
    uint32_t count;      // sem count / event state / cond signal count
    uint32_t maxc;
    uint32_t locked;     // mutex held flag
};
#define SYNC_MAX 32
static OsSyncObj g_sync[SYNC_MAX];

static OsSyncObj* sync_from_handle(h_t h, uint8_t type){
    OsSyncObj* o = (OsSyncObj*)osapi_handle_resolve(h, type, 0);
    return o;
}

static h_t sync_create(uint8_t type, uint32_t init, uint32_t maxc){
    for (int i = 0; i < SYNC_MAX; i++){
        if (g_sync[i].used) continue;
        OsSyncObj* o = &g_sync[i];
        o->used = 1; o->type = type; o->owner = 0;
        o->count = init; o->maxc = maxc; o->locked = 0;
        h_t h = osapi_handle_alloc(type, (uint16_t)i, 0, o, HR_R | HR_W | HR_CTRL);
        return h;
    }
    osapi_set_last_error(ERR_NOSPACE);
    return 0;
}

int32_t osapi_ic_mutex_new(int32_t*){
    return (int32_t)sync_create(HK_MUTEX, 0, 1);
}
int32_t osapi_ic_mutex_lock(int32_t* a){
    OsSyncObj* o = sync_from_handle((h_t)a[0], HK_MUTEX);
    if (!o) return 0;
    if (o->locked && o->owner == g_active_token){
        osapi_set_last_error(ERR_DEADLK); return 0;   // non-recursive
    }
    if (o->locked){ osapi_set_last_error(ERR_BUSY); return 0; }
    o->locked = 1; o->owner = g_active_token;
    osapi_set_last_error(ERR_OK);
    return 1;
}
int32_t osapi_ic_mutex_unlock(int32_t* a){
    OsSyncObj* o = sync_from_handle((h_t)a[0], HK_MUTEX);
    if (!o) return 0;
    if (!o->locked){ osapi_set_last_error(ERR_PERMDENIED); return 0; }
    if (o->owner != g_active_token && g_active_token != 1){
        osapi_set_last_error(ERR_PERMDENIED); return 0;
    }
    o->locked = 0; o->owner = 0;
    osapi_set_last_error(ERR_OK);
    return 1;
}
int32_t osapi_ic_sem_new(int32_t* a){
    if (a[0] < 0 || a[1] <= 0 || a[0] > a[1]){ osapi_set_last_error(ERR_BADARG); return 0; }
    return (int32_t)sync_create(HK_SEM, (uint32_t)a[0], (uint32_t)a[1]);
}
int32_t osapi_ic_sem_wait(int32_t* a){
    OsSyncObj* o = sync_from_handle((h_t)a[0], HK_SEM);
    if (!o) return 0;
    if (o->count == 0){ osapi_set_last_error(ERR_WOULDBLOCK); return 0; }
    o->count--;
    osapi_set_last_error(ERR_OK);
    return 1;
}
int32_t osapi_ic_sem_post(int32_t* a){
    OsSyncObj* o = sync_from_handle((h_t)a[0], HK_SEM);
    if (!o) return 0;
    if (o->count >= o->maxc){ osapi_set_last_error(ERR_BUSY); return 0; }
    o->count++;
    osapi_set_last_error(ERR_OK);
    return 1;
}
int32_t osapi_ic_event_new(int32_t*){
    return (int32_t)sync_create(HK_EVENT, 0, 1);
}
int32_t osapi_ic_event_wait(int32_t* a){
    // a: h, ms, clearOnExit
    OsSyncObj* o = sync_from_handle((h_t)a[0], HK_EVENT);
    if (!o) return 0;
    int ms = (int)a[1];
    uint32_t start = osapi_now_ms();
    for (;;){
        if (o->count){
            if (a[2]) o->count = 0;
            osapi_set_last_error(ERR_OK);
            return 1;
        }
        if (ms <= 0 || (int)(osapi_now_ms() - start) >= ms){
            osapi_set_last_error(ERR_TIMEOUT); return 0;
        }
        spin_wait_ms(1);
    }
}
int32_t osapi_ic_event_set(int32_t* a){
    OsSyncObj* o = sync_from_handle((h_t)a[0], HK_EVENT);
    if (!o) return 0;
    o->count = 1;
    osapi_set_last_error(ERR_OK);
    return 1;
}
int32_t osapi_ic_event_clear(int32_t* a){
    OsSyncObj* o = sync_from_handle((h_t)a[0], HK_EVENT);
    if (!o) return 0;
    o->count = 0;
    osapi_set_last_error(ERR_OK);
    return 1;
}
int32_t osapi_ic_cond_new(int32_t*){
    return (int32_t)sync_create(HK_COND, 0, 0xFFFFFFFFu);
}
int32_t osapi_ic_cond_wait(int32_t* a){
    OsSyncObj* o = sync_from_handle((h_t)a[0], HK_COND);
    if (!o) return 0;
    // cooperative: a cond_wait with no pending signal under a held mutex
    // would spin forever, so refuse outright (caller must re-check state).
    if (o->count == 0){ osapi_set_last_error(ERR_WOULDBLOCK); return 0; }
    o->count--;
    osapi_set_last_error(ERR_OK);
    return 1;
}
int32_t osapi_ic_cond_signal(int32_t* a){
    OsSyncObj* o = sync_from_handle((h_t)a[0], HK_COND);
    if (!o) return 0;
    o->count += (a[1] != 0) ? o->maxc : 1;
    if (o->count > o->maxc) o->count = o->maxc;
    osapi_set_last_error(ERR_OK);
    return 1;
}

// =====================================================================
//  L2  generic async jobs (frame-tickable; Ai/Download migrate here)
// =====================================================================
#define JOB_MAX 16
#define JOB_RUN 0
#define JOB_DONE 1
#define JOB_ERR 2
#define JOB_CANCEL 3
struct OsJob {
    uint8_t used;
    uint8_t status;       // JOB_*
    uint8_t kind;
    uint8_t cancelled;
    uint32_t progress;
    uint32_t total;
    uint32_t ticks;       // work units executed by k_job_tick
    uint32_t goal;        // ticks to complete
};
static OsJob g_jobs[JOB_MAX];

int32_t osapi_ic_job_start(int32_t* a){
    // a: kind, goal
    if (a[1] <= 0){ osapi_set_last_error(ERR_BADARG); return 0; }
    for (int i = 0; i < JOB_MAX; i++){
        if (g_jobs[i].used) continue;
        OsJob* j = &g_jobs[i];
        j->used = 1; j->status = JOB_RUN; j->kind = (uint8_t)a[0];
        j->cancelled = 0; j->progress = 0; j->total = (uint32_t)a[1];
        j->ticks = 0; j->goal = (uint32_t)a[1];
        h_t h = osapi_handle_alloc(HK_JOB, (uint16_t)i, 0, j, HR_R | HR_CTRL);
        return (int32_t)h;
    }
    osapi_set_last_error(ERR_NOSPACE);
    return 0;
}
// called once per frame (mforms_paint_overlay) with a work-unit budget
int32_t osapi_ic_job_tick(int32_t* a){
    int budget = a ? a[0] : 2;
    if (budget <= 0) budget = 1;
    int ran = 0;
    for (int i = 0; i < JOB_MAX; i++){
        if (!g_jobs[i].used || g_jobs[i].status != JOB_RUN) continue;
        OsJob* j = &g_jobs[i];
        if (j->cancelled){ j->status = JOB_CANCEL; continue; }
        for (int b = 0; b < budget && j->ticks < j->goal; b++){
            // built-in job kinds perform their work here; kind 0 = probe job
            j->ticks++;
            j->progress = j->ticks;
        }
        ran++;
        if (j->ticks >= j->goal){
            j->status = (j->cancelled) ? JOB_CANCEL : JOB_DONE;
            j->progress = j->total;
        }
    }
    return ran;
}
static OsJob* job_from(h_t h){
    return (OsJob*)osapi_handle_resolve(h, HK_JOB, 0);
}
int32_t osapi_ic_job_status(int32_t* a){
    OsJob* j = job_from((h_t)a[0]);
    return j ? (int32_t)j->status : -1;
}
int32_t osapi_ic_job_progress(int32_t* a){
    OsJob* j = job_from((h_t)a[0]);
    return j ? (int32_t)j->progress : -1;
}
int32_t osapi_ic_job_progress_total(int32_t* a){
    OsJob* j = job_from((h_t)a[0]);
    return j ? (int32_t)j->total : -1;
}
int32_t osapi_ic_job_cancel(int32_t* a){
    OsJob* j = job_from((h_t)a[0]);
    if (!j) return 0;
    j->cancelled = 1;
    osapi_set_last_error(ERR_OK);
    return 1;
}

// =====================================================================
//  L2  timers
// =====================================================================
#define TIMER_MAX 16
struct OsTimer { uint8_t used; uint32_t period; uint32_t next; uint8_t periodic; };
static OsTimer g_timers[TIMER_MAX];

int32_t osapi_ic_timer_new(int32_t* a){
    if (a[0] <= 0){ osapi_set_last_error(ERR_BADARG); return 0; }
    for (int i = 0; i < TIMER_MAX; i++){
        if (g_timers[i].used) continue;
        OsTimer* t = &g_timers[i];
        t->used = 1;
        t->period = (uint32_t)a[0];
        t->periodic = (uint8_t)(a[1] != 0);
        t->next = osapi_now_ms() + t->period;
        h_t h = osapi_handle_alloc(HK_TIMER, (uint16_t)i, 0, t, HR_R);
        return (int32_t)h;
    }
    osapi_set_last_error(ERR_NOSPACE);
    return 0;
}
int32_t osapi_ic_timer_poll(int32_t* a){
    OsTimer* t = (OsTimer*)osapi_handle_resolve((h_t)a[0], HK_TIMER, 0);
    if (!t) return 0;
    uint32_t now = osapi_now_ms();
    if ((int32_t)(now - t->next) >= 0){
        if (t->periodic) t->next = now + t->period;
        else { t->next = 0xFFFFFFFFu; }   // one-shot: fire once
        return 1;
    }
    return 0;
}

// =====================================================================
//  L3  VFS resolve + file layer on top of the MKFS data volume (fs 0)
//  The 64-bit kernel's fs ops are whole-file (mcp entry points), so the
//  file layer keeps a read/write buffer per open file and commits on
//  Close/Flush via mkfs.create -- giving real Read/Write/Seek semantics
//  plus atomic O_CREAT|O_EXCL.
// =====================================================================
extern "C" int nexos_mcp_fs_list(int fs, char* buf, int bufsize);
extern "C" int nexos_mcp_fs_read(int fs, const char* name, unsigned char* buf, int bufsize);
extern "C" int nexos_mcp_fs_write(int fs, const char* name, const unsigned char* buf, int size);
extern "C" int nexos_mcp_fs_mkdir(int fs, const char* name);
extern "C" int nexos_mcp_fs_remove(int fs, const char* name);
extern "C" int nexos_mcp_fs_rename(int fs, const char* old_name, const char* new_name);
#define FILE_MAX 8
#define FBUF_MAX 4096   // int32 elements == 16 KiB per open file
#define OFLAG_RD 1
#define OFLAG_WR 2
#define OFLAG_CREAT 16
#define OFLAG_EXCL 32
#define OFLAG_TRUNC 8
struct OsFile {
    uint8_t  used;
    char     name[64];
    uint8_t  flags;
    uint32_t pos;
    uint32_t size;
    int      buf[FBUF_MAX];   // int-typed buffer (MiniCLR-side reads use byte icalls)
    uint8_t  dirty;
    uint32_t lock;            // 1 = shared, 2 = exclusive (by token)
    uint32_t lock_owner;
};
static OsFile g_files[FILE_MAX];

// resolve /a/b/../c -> /a/c; returns 0 on ok, error on escaping the root
int osapi_vfs_resolve(const char* in, char* out, int cap){
    if (!in || in[0] != '/'){ if (out && cap>0) out[0]=0; return ERR_BADARG; }
    char comps[16][32];
    int depth = 0;
    int i = 0;
    while (in[i]){
        // skip slashes
        while (in[i] == '/') i++;
        if (!in[i]) break;
        // read one component
        char comp[32]; int n = 0;
        while (in[i] && in[i] != '/'){ if (n < 31) comp[n++] = in[i]; i++; }
        comp[n] = 0;
        if (comp[0] == '.' && comp[1] == 0) continue;          // .
        if (comp[0] == '.' && comp[1] == '.' && comp[2] == 0){
            if (depth == 0) return ERR_NOENTRY;                // .. escapes root
            depth--;
            continue;
        }
        if (depth >= 16) return ERR_TOOBIG;
        int k = 0;
        for (; k < n; k++) comps[depth][k] = comp[k];
        comps[depth][k] = 0;
        depth++;
    }
    int w = 0;
    if (cap < 2) return ERR_TOOBIG;
    out[w++] = '/';
    for (int d = 0; d < depth; d++){
        for (int k = 0; comps[d][k]; k++){
            if (w >= cap - 2) return ERR_TOOBIG;
            out[w++] = comps[d][k];
        }
        if (d + 1 < depth && w < cap - 2) out[w++] = '/';
    }
    out[w] = 0;
    return ERR_OK;
}

int32_t osapi_ic_vfs_resolve(int32_t* a){
    const char* in = clr_str(a[0]);
    if (!in){ osapi_set_last_error(ERR_BADARG); return 0; }
    static char res[128];
    int rc = osapi_vfs_resolve(in, res, sizeof(res));
    if (rc != ERR_OK){ osapi_set_last_error(rc); return 0; }
    return clr_new_str(res);
}

// internal C-string open: canon is already a canonical absolute path.
// flags: OFLAG_*.  Returns handle (0 fail), last error set, *out = file obj.
int osapi_file_open_c(const char* canon, int flags, OsFile** out){
    *out = 0;
    // boundary check (read needs RO grant, create/write needs RW)
    int bmode = (flags & (OFLAG_WR | OFLAG_CREAT | OFLAG_TRUNC)) ? 2 : 1;
    if (!osapi_fs_path_allowed(g_active_token, canon, bmode)) return 0;

    // probe existence: mcp_fs_read into a scratch probe
    static unsigned char probe[1];
    int exists = nexos_mcp_fs_read(0, canon + 1, probe, 1) > 0;

    if ((flags & OFLAG_CREAT) && (flags & OFLAG_EXCL) && exists){
        osapi_set_last_error(ERR_NAMEEXISTS); return 0;   // hard requirement
    }
    if (!(flags & OFLAG_CREAT) && !exists){
        osapi_set_last_error(ERR_NOTFOUND); return 0;
    }
    for (int i = 0; i < FILE_MAX; i++){
        if (g_files[i].used) continue;
        OsFile* f = &g_files[i];
        f->used = 1;
        int k = 0;
        for (; canon[k] && k < 63; k++) f->name[k] = canon[k];
        f->name[k] = 0;
        f->flags = (uint8_t)((flags & OFLAG_WR) ? OFLAG_WR : OFLAG_RD);
        f->pos = 0; f->dirty = 0; f->lock = 0; f->lock_owner = 0;
        // load content
        f->size = 0;
        if (exists && !(flags & OFLAG_TRUNC)){
            unsigned char* bufp = (unsigned char*)f->buf;
            int n = nexos_mcp_fs_read(0, f->name + 1, bufp, FBUF_MAX * 4);
            f->size = (n > 0) ? (uint32_t)n : 0;
        }
        h_t h = osapi_handle_alloc(HK_FILE, (uint16_t)i, 0, f, HR_R | HR_W);
        *out = f;
        return (int)h;
    }
    osapi_set_last_error(ERR_NOSPACE);
    return 0;
}

int32_t osapi_ic_file_open(int32_t* a){
    const char* path = clr_str(a[0]);
    if (!path || path[0] != '/'){ osapi_set_last_error(ERR_BADARG); return 0; }
    static char canon[128];
    int rc = osapi_vfs_resolve(path, canon, sizeof(canon));
    if (rc != ERR_OK){ osapi_set_last_error(rc); return 0; }
    OsFile* f = 0;
    int h = osapi_file_open_c(canon, (int)a[1], &f);
    return (int32_t)h;
}

static OsFile* file_from(h_t h, int need_write){
    OsFile* f = (OsFile*)osapi_handle_resolve(h, HK_FILE, 0);
    if (!f) return 0;
    if (need_write && !(f->flags & OFLAG_WR)){ osapi_set_last_error(ERR_PERMDENIED); return 0; }
    return f;
}

int32_t osapi_ic_file_read(int32_t* a){
    OsFile* f = file_from((h_t)a[0], 0);
    if (!f) return 0;
    int32_t* dst = clr_arr_data(a[1]);
    int alen = clr_arr_len(a[1]);
    int len = (int)a[2];
    if (len > alen) len = alen;                 // clamp to managed array
    if (!dst || len < 0){ osapi_set_last_error(ERR_BADARG); return 0; }
    int avail = (int)(f->size - f->pos);
    if (avail <= 0){ osapi_set_last_error(ERR_OK); return 0; }
    int n = len < avail ? len : avail;
    const uint8_t* src = (const uint8_t*)f->buf + f->pos;
    for (int i = 0; i < n; i++) dst[i] = src[i];
    f->pos += (uint32_t)n;
    osapi_set_last_error(ERR_OK);
    return n;
}

int32_t osapi_ic_file_write(int32_t* a){
    OsFile* f = file_from((h_t)a[0], 1);
    if (!f) return 0;
    const int32_t* src = clr_arr_data(a[1]);
    int alen = clr_arr_len(a[1]);
    int len = (int)a[2];
    if (len > alen) len = alen;
    if (!src || len < 0){ osapi_set_last_error(ERR_BADARG); return 0; }
    if ((uint32_t)(f->pos + len) > FBUF_MAX){ osapi_set_last_error(ERR_TOOBIG); return 0; }
    uint8_t* dst = (uint8_t*)f->buf + f->pos;
    for (int i = 0; i < len; i++) dst[i] = (uint8_t)src[i];
    f->pos += (uint32_t)len;
    if (f->pos > f->size) f->size = f->pos;
    f->dirty = 1;
    osapi_set_last_error(ERR_OK);
    return len;
}

int32_t osapi_ic_file_seek(int32_t* a){
    OsFile* f = file_from((h_t)a[0], 0);
    if (!f) return 0;
    int off = (int)a[1];
    int whence = (int)a[2];
    uint32_t target = f->pos;
    if (whence == 0) target = (uint32_t)off;               // SET
    else if (whence == 1) target = f->pos + (uint32_t)off; // CUR
    else if (whence == 2) target = (uint32_t)((int)f->size + off); // END
    else { osapi_set_last_error(ERR_BADARG); return 0; }
    if (target > FBUF_MAX){ osapi_set_last_error(ERR_TOOBIG); return 0; }
    f->pos = target;
    return (int32_t)target;
}

int32_t osapi_ic_file_size(int32_t* a){
    OsFile* f = file_from((h_t)a[0], 0);
    if (!f) return 0;
    return (int32_t)f->size;
}

int osapi_file_commit(OsFile* f){
    if (!f->dirty) return ERR_OK;
    int r = nexos_mcp_fs_write(0, f->name + 1, (const unsigned char*)f->buf, (int)f->size);
    if (r < 0){ return ERR_IO; }
    f->dirty = 0;
    return ERR_OK;
}

int32_t osapi_ic_file_flush(int32_t* a){
    OsFile* f = file_from((h_t)a[0], 1);
    if (!f) return 0;
    int rc = osapi_file_commit(f);
    if (rc != ERR_OK){ osapi_set_last_error(rc); return 0; }
    osapi_set_last_error(ERR_OK);
    return 1;
}

int32_t osapi_ic_file_close(int32_t* a){
    OsFile* f = file_from((h_t)a[0], 0);
    if (!f) return 0;
    int rc = osapi_file_commit(f);
    f->used = 0;
    osapi_handle_close((h_t)a[0], 0);
    if (rc != ERR_OK){ osapi_set_last_error(rc); return 0; }
    osapi_set_last_error(ERR_OK);
    return 1;
}

int32_t osapi_ic_file_rename(int32_t* a){
    const char* o = clr_str(a[0]);
    const char* n = clr_str(a[1]);
    if (!o || !n){ osapi_set_last_error(ERR_BADARG); return 0; }
    static char co[128]; static char cn[128];
    if (osapi_vfs_resolve(o, co, sizeof(co)) != ERR_OK){ osapi_set_last_error(ERR_BADARG); return 0; }
    if (osapi_vfs_resolve(n, cn, sizeof(cn)) != ERR_OK){ osapi_set_last_error(ERR_BADARG); return 0; }
    if (!osapi_fs_path_allowed(g_active_token, co, 2) ||
        !osapi_fs_path_allowed(g_active_token, cn, 2)) return 0;
    // rename = read old + write new + remove old
    static unsigned char buf[8192];
    int sz = nexos_mcp_fs_read(0, co + 1, buf, sizeof(buf));
    if (sz < 0){ osapi_set_last_error(ERR_NOTFOUND); return 0; }
    if (nexos_mcp_fs_write(0, cn + 1, buf, sz) < 0){ osapi_set_last_error(ERR_IO); return 0; }
    if (nexos_mcp_fs_remove(0, co + 1) < 0){ osapi_set_last_error(ERR_IO); return 0; }
    osapi_set_last_error(ERR_OK);
    return 1;
}

int32_t osapi_ic_file_unlink(int32_t* a){
    const char* p = clr_str(a[0]);
    if (!p){ osapi_set_last_error(ERR_BADARG); return 0; }
    static char c[128];
    if (osapi_vfs_resolve(p, c, sizeof(c)) != ERR_OK){ osapi_set_last_error(ERR_BADARG); return 0; }
    if (!osapi_fs_path_allowed(g_active_token, c, 2)) return 0;
    if (nexos_mcp_fs_remove(0, c + 1) < 0){ osapi_set_last_error(ERR_NOTFOUND); return 0; }
    osapi_set_last_error(ERR_OK);
    return 1;
}

int32_t osapi_ic_file_mkdir(int32_t* a){
    const char* p = clr_str(a[0]);
    if (!p){ osapi_set_last_error(ERR_BADARG); return 0; }
    static char c[128];
    if (osapi_vfs_resolve(p, c, sizeof(c)) != ERR_OK){ osapi_set_last_error(ERR_BADARG); return 0; }
    if (!osapi_fs_path_allowed(g_active_token, c, 2)) return 0;
    if (nexos_mcp_fs_mkdir(0, c + 1) < 0){ osapi_set_last_error(ERR_NAMEEXISTS); return 0; }
    osapi_set_last_error(ERR_OK);
    return 1;
}

int32_t osapi_ic_dir_open(int32_t* a){
    const char* p = clr_str(a[0]);
    if (!p){ osapi_set_last_error(ERR_BADARG); return 0; }
    static char c[128];
    if (osapi_vfs_resolve(p, c, sizeof(c)) != ERR_OK){ osapi_set_last_error(ERR_BADARG); return 0; }
    if (!osapi_fs_path_allowed(g_active_token, c, 1)) return 0;
    // list once into a dir cache entry
    static struct { uint8_t used; char data[4096]; int pos; } dcaches[4];
    for (int i = 0; i < 4; i++){
        if (dcaches[i].used) continue;
        dcaches[i].used = 1;
        dcaches[i].pos = 0;
        int n = nexos_mcp_fs_list(0, dcaches[i].data, sizeof(dcaches[i].data));
        if (n < 0) dcaches[i].data[0] = 0;
        h_t h = osapi_handle_alloc(HK_DIR, (uint16_t)i, 0, &dcaches[i], HR_R);
        return (int32_t)h;
    }
    osapi_set_last_error(ERR_NOSPACE);
    return 0;
}

int32_t osapi_ic_dir_next(int32_t* a){
    typedef struct { uint8_t used; char data[4096]; int pos; } DCache;
    DCache* d = (DCache*)osapi_handle_resolve((h_t)a[0], HK_DIR, 0);
    if (!d) return 0;
    const char* p = d->data + d->pos;
    if (!*p) return 0;                       // end of listing (Err stays OK)
    // one entry per line
    int n = 0;
    while (p[n] && p[n] != '\n') n++;
    static char ent[64];
    int k = (n < 63) ? n : 63;
    for (int i = 0; i < k; i++) ent[i] = p[i];
    ent[k] = 0;
    d->pos += (p[n] == '\n') ? (n + 1) : n;
    return clr_new_str(ent);
}

int32_t osapi_ic_file_lock(int32_t* a){
    OsFile* f = file_from((h_t)a[0], 0);
    if (!f) return 0;
    int op = (int)a[1];
    if (op == 0){                              // UNLOCK
        if (f->lock && f->lock_owner != g_active_token && g_active_token != 1){
            osapi_set_last_error(ERR_PERMDENIED); return 0;
        }
        f->lock = 0; f->lock_owner = 0;
        osapi_set_last_error(ERR_OK); return 1;
    }
    if (op == 1 || op == 2){                   // LOCK_SH / LOCK_EX
        if (f->lock && f->lock_owner != g_active_token){
            osapi_set_last_error(ERR_BUSY); return 0;
        }
        f->lock = (uint32_t)op;
        f->lock_owner = g_active_token;
        osapi_set_last_error(ERR_OK); return 1;
    }
    osapi_set_last_error(ERR_BADARG);
    return 0;
}

int32_t osapi_ic_l2l3_self_test(int32_t*);
int32_t osapi_ic_l2l3_self_test(int32_t*){
    int fails = 0;
    // ===================== L2/L3 self-test =====================
    int f2 = 0;

    // -- mutex lifecycle + double lock + wrong unlock
    h_t m = osapi_ic_mutex_new(0);
    {
        int32_t la[1]; la[0] = (int32_t)m;
        if (!osapi_ic_mutex_lock(la)) { f2++; oser("  mutex lock failed\n"); }
        if (osapi_ic_mutex_lock(la)) { f2++; oser("  recursive lock accepted\n"); }
        if (osapi_get_last_error() != ERR_DEADLK) { f2++; oser("  deadlock err missing\n"); }
        if (!osapi_ic_mutex_unlock(la)) { f2++; oser("  mutex unlock failed\n"); }
    }
    // -- sem
    {
        int32_t sa[2]; sa[0] = 1; sa[1] = 2;
        h_t s = osapi_ic_sem_new(sa);
        int32_t one[1]; one[0] = (int32_t)s;
        if (!osapi_ic_sem_wait(one)) { f2++; oser("  sem wait failed\n"); }
        if (osapi_ic_sem_wait(one)) { f2++; oser("  sem over-wait accepted\n"); }
        if (!osapi_ic_sem_post(one)) { f2++; oser("  sem post failed\n"); }
    }
    // -- event: set then bounded wait succeeds; cleared wait times out
    {
        h_t e = osapi_ic_event_new(0);
        int32_t h1[1]; h1[0] = (int32_t)e;
        int32_t seta[1]; seta[0] = (int32_t)e;
        osapi_ic_event_set(seta);
        int32_t wa[3]; wa[0] = (int32_t)e; wa[1] = 5; wa[2] = 1;
        if (!osapi_ic_event_wait(wa)) { f2++; oser("  event wait after set failed\n"); }
        wa[1] = 2;
        if (osapi_ic_event_wait(wa)) { f2++; oser("  cleared event wait accepted\n"); }
        if (osapi_get_last_error() != ERR_TIMEOUT) { f2++; oser("  event timeout err missing\n"); }
    }
    // -- job: start -> tick to completion -> cancel semantics
    {
        int32_t ja[2]; ja[0] = 0; ja[1] = 10;
        h_t j = osapi_ic_job_start(ja);
        int32_t h1[1]; h1[0] = (int32_t)j;
        osapi_ic_job_tick(0);          // budget default 2 -> 2/10
        if (osapi_ic_job_progress(h1) != 2) { f2++; oser("  job progress wrong\n"); }
        osapi_ic_job_tick(0); osapi_ic_job_tick(0); osapi_ic_job_tick(0);
        osapi_ic_job_tick(0);          // total 10 ticks -> done
        if (osapi_ic_job_status(h1) != JOB_DONE) { f2++; oser("  job not DONE\n"); }
        // cancelled job
        h_t j2 = osapi_ic_job_start(ja);
        int32_t h2[1]; h2[0] = (int32_t)j2;
        osapi_ic_job_cancel(h2);
        osapi_ic_job_tick(0);
        if (osapi_ic_job_status(h2) != JOB_CANCEL) { f2++; oser("  job cancel lost\n"); }
    }
    // -- timer: one-shot fires after period
    {
        int32_t ta[2]; ta[0] = 3; ta[1] = 0;
        h_t t = osapi_ic_timer_new(ta);
        int32_t h1[1]; h1[0] = (int32_t)t;
        if (osapi_ic_timer_poll(h1)) { f2++; oser("  timer fired early\n"); }
        spin_wait_ms(6);
        if (!osapi_ic_timer_poll(h1)) { f2++; oser("  timer never fired\n"); }
    }
    // -- vfs resolve: .. popping and root escape
    {
        char out[128];
        if (osapi_vfs_resolve("/a/b/../c", out, sizeof(out)) != ERR_OK ||
            !ostr_eq(out, "/a/c")) { f2++; oser("  vfs .. resolve wrong\n"); }
        if (osapi_vfs_resolve("/../x", out, sizeof(out)) != ERR_NOENTRY) {
            f2++; oser("  root escape accepted\n");
        }
        if (osapi_vfs_resolve("/a//b/./c", out, sizeof(out)) != ERR_OK ||
            !ostr_eq(out, "/a/b/c")) { f2++; oser("  vfs normalize wrong\n"); }
    }
    // -- file layer (C API, no CLR needed): O_CREAT|O_EXCL, round trip
    {
        OsFile* f = 0;
        int h1 = osapi_file_open_c("/ost1.txt", OFLAG_CREAT|OFLAG_EXCL|OFLAG_WR, &f);
        if (!h1) {
            f2++;
            oser("  first O_CREAT|EXCL failed err=");
            oser_dec(osapi_get_last_error());
            oser("\n");
        } else {
            // write 4 bytes and commit so the file REALLY exists on disk,
            // then the duplicate O_CREAT|O_EXCL must fail with NameExists.
            f->buf[0]='A'; f->buf[1]='B'; f->buf[2]='C'; f->buf[3]='D';
            f->pos = 4; f->size = 4; f->dirty = 1;
            if (osapi_file_commit(f) != ERR_OK) { f2++; oser("  commit failed\n"); }
            OsFile* f2o = 0;
            int h2 = osapi_file_open_c("/ost1.txt", OFLAG_CREAT|OFLAG_EXCL|OFLAG_WR, &f2o);
            if (h2 || osapi_get_last_error() != ERR_NAMEEXISTS) {
                f2++; oser("  O_EXCL did not reject duplicate\n");
            } else {
                osapi_handle_close((h_t)h2, 0); if (f2o) f2o->used = 0;
            }
            osapi_handle_close((h_t)h1, 0); f->used = 0;
            OsFile* fr = 0;
            int h3 = osapi_file_open_c("/ost1.txt", OFLAG_RD, &fr);
            if (!h3) { f2++; oser("  reopen failed\n"); }
            else {
                if (fr->size != 4 || fr->buf[0] != 'A' || fr->buf[3] != 'D') {
                    f2++; oser("  file round trip mismatch\n");
                }
                osapi_handle_close((h_t)h3, 0); fr->used = 0;
            }
            extern int nexos_mcp_fs_remove(int fs, const char* name);
            nexos_mcp_fs_remove(0, "ost1.txt");
        }
    }
    oser("[OSAPI] l2l3 self-test fails=");
    oser_dec(f2);
    oser("\n");
    return fails + f2;
}

// =====================================================================
//  L2 processes: cooperative CLR-slot processes.
//  Spawn  = load the .mex into a free CLR slot and run App.Main::Init.
//  Exit   = managed Proc.Exit marks the slot EXITED; the slot memory is
//           reaped later (job tick / Wait / Kill) because we cannot free
//           the slot we are currently executing in.
//  Kill   = immediate clr_close_slot (caller runs in another slot).
//  Wait   = poll (no preemption on this kernel): returns -1 while the
//           process is still running, otherwise the exit code (>=0).
// =====================================================================
#define PROC_MAX 8
struct OsProc {
    uint8_t used;
    uint8_t state;          // 0 running, 1 exited, 2 failed
    int     slot;           // CLR slot, -1 once reaped
    int     exit_code;
    uint32_t token;         // owner token
    char    mex[32];
};
static OsProc g_procs[PROC_MAX];

#define PS_RUNNING 0
#define PS_EXITED  1
#define PS_FAILED  2

// reap one exited process (free its CLR slot); called from tick/wait/kill
static void osapi_proc_reap(OsProc* p){
    if (!p->used || p->state == PS_RUNNING) return;
    if (p->slot > 0) { clr_close_slot(p->slot); p->slot = -1; }
    p->used = 0;
}

// internal C API: mex is the image basename, e.g. "Notepad.mex".
// Returns pid (1-based) or 0 with last error set.
int osapi_proc_spawn_c(const char* mex, uint32_t token){
    if (!mex || !mex[0]){ osapi_set_last_error(ERR_BADARG); return 0; }
    if (!osapi_dev_allowed(token, 6, 0)){      // DEV_PROC class 6
        osapi_set_last_error(ERR_PERMDENIED); return 0;
    }
    OsProc* p = 0;
    for (int i = 0; i < PROC_MAX; i++){
        if (!g_procs[i].used){ p = &g_procs[i]; break; }
    }
    if (!p){ osapi_set_last_error(ERR_NOSPACE); return 0; }
    int slot = -1;
    for (int s = 1; s < CLR_MAX_VMS; s++)
        if (!clr_slot_loaded(s)){ slot = s; break; }
    if (slot < 0){ osapi_set_last_error(ERR_BUSY); return 0; }
    int prev = clr_use(slot);
    int rc = clr_load_slot(slot, mex);
    if (rc < 0){
        clr_use(prev);
        clr_close_slot(slot);
        osapi_set_last_error(ERR_NOTFOUND);
        return 0;
    }
    int32_t r = 0;
    if (clr_call("App.Main::Init", 0, 0, &r) != 0){
        clr_use(prev);
        clr_close_slot(slot);
        osapi_set_last_error(ERR_IO);
        return 0;
    }
    clr_use(prev);
    p->used = 1; p->state = PS_RUNNING; p->slot = slot;
    p->exit_code = 0; p->token = token;
    int k = 0;
    for (; mex[k] && k < 31; k++) p->mex[k] = mex[k];
    p->mex[k] = 0;
    osapi_set_last_error(ERR_OK);
    return (int)(p - g_procs) + 1;
}

static OsProc* proc_from_pid(int pid){
    if (pid < 1 || pid > PROC_MAX) return 0;
    OsProc* p = &g_procs[pid - 1];
    return (p->used && p->token == g_active_token) ? p : 0;
}

int32_t osapi_ic_proc_spawn(int32_t* a){
    const char* mex = clr_str(a[0]);
    int pid = osapi_proc_spawn_c(mex, g_active_token);
    return pid;
}

// Wait: cooperative poll. ret <0 = still running; >=0 exit code.
int32_t osapi_ic_proc_wait(int32_t* a){
    OsProc* p = proc_from_pid((int)a[0]);
    if (!p){ osapi_set_last_error(ERR_BADHANDLE); return -2; }
    if (p->state == PS_RUNNING){ osapi_set_last_error(ERR_OK); return -1; }
    int code = (p->state == PS_EXITED) ? p->exit_code : -3;
    osapi_proc_reap(p);
    osapi_set_last_error(ERR_OK);
    return code;
}

int32_t osapi_ic_proc_kill(int32_t* a){
    OsProc* p = proc_from_pid((int)a[0]);
    if (!p){ osapi_set_last_error(ERR_BADHANDLE); return 0; }
    if (p->slot > 0) clr_close_slot(p->slot);
    p->slot = -1; p->state = PS_EXITED; p->exit_code = -4;   // killed
    p->used = 0;
    osapi_set_last_error(ERR_OK);
    return 1;
}

int32_t osapi_ic_proc_exit(int32_t* a){
    // runs INSIDE the caller's slot: find own process by slot
    int cur = clr_current_slot();
    for (int i = 0; i < PROC_MAX; i++){
        OsProc* p = &g_procs[i];
        if (p->used && p->slot == cur && p->state == PS_RUNNING){
            p->state = PS_EXITED;
            p->exit_code = (int)a[0];
            osapi_set_last_error(ERR_OK);
            return 1;
        }
    }
    osapi_set_last_error(ERR_BADHANDLE);
    return 0;
}

int32_t osapi_ic_proc_count(int32_t* a){
    int n = 0;
    for (int i = 0; i < PROC_MAX; i++) if (g_procs[i].used) n++;
    return n;
}

int32_t osapi_ic_proc_pid_at(int32_t* a){
    int want = (int)a[0];
    int n = 0;
    for (int i = 0; i < PROC_MAX; i++){
        if (!g_procs[i].used) continue;
        if (n++ == want) return i + 1;
    }
    return 0;
}

int32_t osapi_ic_proc_state_at(int32_t* a){
    OsProc* p = proc_from_pid((int)a[0]);
    return p ? p->state : -1;
}

// reap pending exits on every frame tick (piggybacks the job tick icall)
int32_t osapi_ic_proc_tick(int32_t* a){
    for (int i = 0; i < PROC_MAX; i++) osapi_proc_reap(&g_procs[i]);
    osapi_set_last_error(ERR_OK);
    return 0;
}

// =====================================================================
//  L3 glob: '*' any run, '?' one char.  Pattern applies to one path
//  segment level already handled by the caller (we match whole names).
//  Fs.Glob(dir, pattern) -> string[] as newline-joined managed string.
// =====================================================================
static int glob_match(const char* pat, const char* s){
    // iterative backtracking, no recursion (kernel space is tight)
    const char* star = 0; const char* ss = 0;
    while (*s){
        if (*pat == '?' || *pat == *s){ pat++; s++; }
        else if (*pat == '*'){ star = pat++; ss = s; }
        else if (star){ pat = star + 1; s = ++ss; }
        else return 0;
    }
    while (*pat == '*') pat++;
    return *pat == 0;
}

// C-side glob, independent of the CLR string allocator.  Splitting this out
// matters: clr_new_str() returns 0 outside a CLR context, so a self-test that
// called only the icall was silently SKIPPING its glob assertions (a false
// green on the first mforms_init pass, a failure on the later in-CLR pass).
// Returns bytes written, or -1 BADARG / -2 PERMDENIED / -3 IO.
static int osapi_fs_glob_c(const char* dir, const char* pat,
                           char* out, int outsz){
    if (!dir || !pat || !out || outsz <= 0) return -1;
    static char cd[128];
    if (osapi_vfs_resolve(dir, cd, sizeof(cd)) != ERR_OK) return -1;
    if (!osapi_fs_path_allowed(g_active_token, cd, 1)) return -2;
    char list[4096];
    list[0] = 0;
    int n = nexos_mcp_fs_list(0, list, sizeof(list));
    if (n < 0) return -3;
    // NOTE: root-only listing in v1 (fs_list has no dir argument); the
    // dir prefix is applied to the pattern check below.
    // CRITICAL: nexos_mcp_fs_list() -> gui_cb_list_files() returns the NUMBER
    // OF ENTRIES, not the byte length of the buffer (it returns `count`,
    // while `pos` holds the written length).  The buffer is NUL-terminated,
    // so scan to the terminator.  Using `n` as a byte count truncated the
    // scan to the first few bytes, so Glob could never see any file past the
    // first entry -- including the fixture the self-test had just created.
    int o = 0;
    int i = 0;
    while (i < (int)sizeof(list) - 1 && list[i]){
        int len = 0;
        while (list[i + len] && list[i + len] != '\n') len++;
        char name[64];
        int k = (len < 63) ? len : 63;
        for (int j = 0; j < k; j++) name[j] = list[i + j];
        name[k] = 0;
        // strip the listing prefix ("    " file / "[D] " dir)
        char* nm = name;
        if (k >= 4 && (name[0] == '[' || name[0] == ' ')){
            nm = name + 4;
            k -= 4;
        }
        if (glob_match(pat, nm) && o + k + 2 < outsz){
            for (int j = 0; j < k; j++) out[o++] = nm[j];
            out[o++] = '\n';
        }
        i += len + 1;
    }
    out[o] = 0;
    return o;
}

// returns newline-joined match list as managed string (0 on error)
int32_t osapi_ic_fs_glob(int32_t* a){
    const char* dir = clr_str(a[0]);
    const char* pat = clr_str(a[1]);
    if (!dir || !pat){ osapi_set_last_error(ERR_BADARG); return 0; }
    static char out[4096];
    int r = osapi_fs_glob_c(dir, pat, out, sizeof(out));
    if (r < 0){
        osapi_set_last_error(r == -1 ? ERR_BADARG :
                             r == -2 ? ERR_PERMDENIED : ERR_IO);
        return 0;
    }
    osapi_set_last_error(ERR_OK);
    return clr_new_str(out);
}

// =====================================================================
//  L12  configuration store
// ---------------------------------------------------------------------
//  Everything below is a PURE C API.  No clr_str()/clr_new_str() calls:
//  the kernel self-test runs outside a managed context, where clr_new_str
//  returns 0 and would silently skip every assertion (that was the bug in
//  the first Glob self-test).  The icalls at the bottom are thin adapters.
// =====================================================================
static OsCfg g_cfgs[CFG_MAX];
static char  g_cfg_io[CFG_FILE_MAX];   // shared serialize/parse scratch

static int cfg_len(const char* s){ int n = 0; if (s) while (s[n]) n++; return n; }
static void cfg_cpy(char* d, const char* s, int cap){
    int i = 0;
    if (s) for (; s[i] && i < cap - 1; i++) d[i] = s[i];
    d[i] = 0;
}

// Logical names are single path components: [A-Za-z0-9_.-], <=15 chars,
// never "." / "..".  This is what makes the canonical /data/cfg/<name>.cfg
// prefix check meaningful (segment boundaries) on a flat FS.
static int cfg_name_ok(const char* s){
    if (!s || !s[0]) return 0;
    int n = 0;
    for (int i = 0; s[i]; i++){
        char c = s[i];
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                 (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
        if (!ok) return 0;                       // '/' and '\\' rejected
        if (++n >= CFG_NAME_MAX) return 0;       // "<name>.cfg" <= 19 bytes
    }
    if (n == 1 && s[0] == '.') return 0;
    if (n == 2 && s[0] == '.' && s[1] == '.') return 0;
    return n;
}

static void cfg_basename(const char* name, const char* ext, char* out){
    int o = 0, i = 0;
    while (name[i]) out[o++] = name[i++];
    out[o++] = '.';
    i = 0; while (ext[i]) out[o++] = ext[i++];
    out[o] = 0;
}
static void cfg_canonical(const char* base, char* out){
    const char* pre = "/data/cfg/";
    int o = 0, i = 0;
    while (pre[i]) out[o++] = pre[i++];
    i = 0; while (base[i]) out[o++] = base[i++];
    out[o] = 0;
}

static OsCfg* cfg_from(h_t h){
    return (OsCfg*)osapi_handle_resolve(h, HK_CFG, g_active_token);
}

static int cfg_key_ok(const char* k){
    int n = cfg_len(k);
    if (n <= 0 || n >= CFG_KEY_MAX) return 0;
    for (int i = 0; k[i]; i++)
        if (k[i] == '=' || k[i] == '\n' || k[i] == '\r' || k[i] == '/') return 0;
    return 1;
}
static int cfg_val_ok(const char* v){
    int n = cfg_len(v);
    if (n < 0 || n >= CFG_VAL_MAX) return 0;
    for (int i = 0; v[i]; i++)
        if (v[i] == '\n' || v[i] == '\r') return 0;
    return 1;
}

static int cfg_find(const OsCfg* c, const char* key){
    for (int i = 0; i < c->n; i++)
        if (ostr_eq(c->e[i].key, key)) return i;
    return -1;
}

// Parse "key=value\n" lines into the in-memory copy.  Malformed / overlong
// lines are skipped rather than corrupting the table.
static void cfg_parse(OsCfg* c, const char* txt, int len){
    c->n = 0;
    int i = 0;
    while (i < len && txt[i]){
        int ls = i;
        while (i < len && txt[i] && txt[i] != '\n') i++;
        int le = i;
        if (i < len && txt[i] == '\n') i++;
        int eq = -1;
        for (int k = ls; k < le; k++) if (txt[k] == '='){ eq = k; break; }
        if (eq <= ls) continue;                       // no key
        int kl = eq - ls, vl = le - eq - 1;
        if (kl <= 0 || kl >= CFG_KEY_MAX || vl < 0 || vl >= CFG_VAL_MAX) continue;
        if (c->n >= CFG_MAX_KEYS) break;
        CfgEntry* e = &c->e[c->n];
        for (int k = 0; k < kl; k++) e->key[k] = txt[ls + k];
        e->key[kl] = 0;
        for (int k = 0; k < vl; k++) e->val[k] = txt[eq + 1 + k];
        e->val[vl] = 0;
        c->n++;
    }
}

h_t osapi_cfg_open_c(const char* name){
    if (!cfg_name_ok(name)){ osapi_set_last_error(ERR_BADARG); return 0; }
    char base[CFG_NAME_MAX + 4];
    cfg_basename(name, "cfg", base);
    char path[64];
    cfg_canonical(base, path);
    // Read grant on the logical path (segment-boundary aware).
    if (!osapi_fs_path_allowed(g_active_token, path, 1)) return 0;

    for (int i = 0; i < CFG_MAX; i++){
        if (g_cfgs[i].used) continue;
        OsCfg* c = &g_cfgs[i];
        c->used = 1; c->dirty = 0; c->n = 0;
        c->owner = g_active_token;
        cfg_cpy(c->name, name, CFG_NAME_MAX);
        cfg_cpy(c->path, path, (int)sizeof(c->path));
        for (int k = 0; k < CFG_MAX_KEYS; k++){ c->e[k].key[0] = 0; c->e[k].val[0] = 0; }
        int n = nexos_mcp_fs_read(0, base, (unsigned char*)g_cfg_io, CFG_FILE_MAX);
        if (n > 0) cfg_parse(c, g_cfg_io, n);
        h_t h = osapi_handle_alloc(HK_CFG, (uint16_t)i, g_active_token,
                                   c, HR_R | HR_W);
        if (!h){ c->used = 0; return 0; }
        osapi_set_last_error(ERR_OK);
        return h;
    }
    osapi_set_last_error(ERR_NOSPACE);
    return 0;
}

int osapi_cfg_get_c(h_t h, const char* key, char* out, int cap){
    OsCfg* c = cfg_from(h);
    if (!c) return osapi_get_last_error();
    if (!cfg_key_ok(key) || !out || cap <= 0){ osapi_set_last_error(ERR_BADARG); return ERR_BADARG; }
    int idx = cfg_find(c, key);
    if (idx < 0){ osapi_set_last_error(ERR_NOENTRY); return ERR_NOENTRY; }
    cfg_cpy(out, c->e[idx].val, cap);
    osapi_set_last_error(ERR_OK);
    return ERR_OK;
}

int osapi_cfg_set_c(h_t h, const char* key, const char* val){
    OsCfg* c = cfg_from(h);
    if (!c) return osapi_get_last_error();
    if (!cfg_key_ok(key)){ osapi_set_last_error(ERR_TOOBIG); return ERR_TOOBIG; }
    if (!cfg_val_ok(val)){ osapi_set_last_error(ERR_TOOBIG); return ERR_TOOBIG; }
    int idx = cfg_find(c, key);
    if (idx >= 0){
        cfg_cpy(c->e[idx].val, val, CFG_VAL_MAX);
    } else {
        if (c->n >= CFG_MAX_KEYS){ osapi_set_last_error(ERR_NOSPACE); return ERR_NOSPACE; }
        cfg_cpy(c->e[c->n].key, key, CFG_KEY_MAX);
        cfg_cpy(c->e[c->n].val, val, CFG_VAL_MAX);
        c->n++;
    }
    c->dirty = 1;
    osapi_set_last_error(ERR_OK);
    return ERR_OK;
}

int osapi_cfg_del_c(h_t h, const char* key){
    OsCfg* c = cfg_from(h);
    if (!c) return osapi_get_last_error();
    if (!cfg_key_ok(key)){ osapi_set_last_error(ERR_BADARG); return ERR_BADARG; }
    int idx = cfg_find(c, key);
    if (idx < 0){ osapi_set_last_error(ERR_NOENTRY); return ERR_NOENTRY; }
    for (int i = idx; i + 1 < c->n; i++) c->e[i] = c->e[i + 1];
    c->n--;
    c->dirty = 1;
    osapi_set_last_error(ERR_OK);
    return ERR_OK;
}

int osapi_cfg_keys_c(h_t h, int index, char* out, int cap){
    OsCfg* c = cfg_from(h);
    if (!c) return osapi_get_last_error();
    if (index < 0 || index >= c->n || !out || cap <= 0){
        osapi_set_last_error(index >= 0 ? ERR_NOENTRY : ERR_BADARG);
        return index >= 0 ? ERR_NOENTRY : ERR_BADARG;
    }
    cfg_cpy(out, c->e[index].key, cap);
    osapi_set_last_error(ERR_OK);
    return ERR_OK;
}

// Atomic at the FS layer: write "<name>.tmp" then rename to "<name>.cfg".
// mkfs.create() overwrites an existing destination, so rename is a single
// copy+remove; a crash between the two leaves the previous .cfg intact.
int osapi_cfg_flush_c(h_t h){
    OsCfg* c = cfg_from(h);
    if (!c) return osapi_get_last_error();
    if (!osapi_fs_path_allowed(g_active_token, c->path, 2)) return osapi_get_last_error();

    int o = 0;
    for (int i = 0; i < c->n; i++){
        int kl = cfg_len(c->e[i].key), vl = cfg_len(c->e[i].val);
        if (o + kl + 1 + vl + 1 >= CFG_FILE_MAX){
            osapi_set_last_error(ERR_TOOBIG); return ERR_TOOBIG;
        }
        for (int k = 0; k < kl; k++) g_cfg_io[o++] = c->e[i].key[k];
        g_cfg_io[o++] = '=';
        for (int k = 0; k < vl; k++) g_cfg_io[o++] = c->e[i].val[k];
        g_cfg_io[o++] = '\n';
    }
    char base[CFG_NAME_MAX + 4], tmp[CFG_NAME_MAX + 4];
    cfg_basename(c->name, "cfg", base);
    cfg_basename(c->name, "tmp", tmp);
    if (nexos_mcp_fs_write(0, tmp, (const unsigned char*)g_cfg_io, o) < 0){
        osapi_set_last_error(ERR_IO); return ERR_IO;
    }
    if (nexos_mcp_fs_rename(0, tmp, base) < 0){
        osapi_set_last_error(ERR_IO); return ERR_IO;
    }
    c->dirty = 0;
    osapi_set_last_error(ERR_OK);
    return ERR_OK;
}

// Close flushes first (so an unflushed Set is not silently lost), then
// releases the handle with gen++ so a stale handle resolves to Err.Stale.
int osapi_cfg_close_c(h_t h){
    OsCfg* c = cfg_from(h);
    if (!c) return osapi_get_last_error();
    int rc = c->dirty ? osapi_cfg_flush_c(h) : ERR_OK;
    c->used = 0;
    osapi_handle_close(h, g_active_token);
    if (rc != ERR_OK) return rc;
    osapi_set_last_error(ERR_OK);
    return ERR_OK;
}

int osapi_cfg_count_c(h_t h){
    OsCfg* c = cfg_from(h);
    if (!c) return -osapi_get_last_error();
    osapi_set_last_error(ERR_OK);
    return c->n;
}
int osapi_cfg_dirty_c(h_t h){
    OsCfg* c = cfg_from(h);
    if (!c) return -osapi_get_last_error();
    osapi_set_last_error(ERR_OK);
    return c->dirty ? 1 : 0;
}

// ---- managed icalls (thin adapters, GUARD on every entry) --------------
int32_t osapi_ic_cfg_open(int32_t* a){
    ICALL_GUARD(CAP_CFG, SCL_MISC);
    const char* name = clr_str(a[0]);
    if (!name){ osapi_set_last_error(ERR_BADARG); return 0; }
    return (int32_t)osapi_cfg_open_c(name);
}
int32_t osapi_ic_cfg_get(int32_t* a){
    ICALL_GUARD(CAP_CFG, SCL_MISC);
    const char* key = clr_str(a[1]);
    if (!key){ osapi_set_last_error(ERR_BADARG); return 0; }
    static char out[CFG_VAL_MAX];
    int rc = osapi_cfg_get_c((h_t)a[0], key, out, sizeof(out));
    if (rc != ERR_OK) return 0;                  // null => no such key
    return clr_new_str(out);
}
int32_t osapi_ic_cfg_set(int32_t* a){
    ICALL_GUARD(CAP_CFG, SCL_MISC);
    const char* key = clr_str(a[1]);
    const char* val = clr_str(a[2]);
    if (!key || !val){ osapi_set_last_error(ERR_BADARG); return 0; }
    return osapi_cfg_set_c((h_t)a[0], key, val) == ERR_OK ? 1 : 0;
}
int32_t osapi_ic_cfg_del(int32_t* a){
    ICALL_GUARD(CAP_CFG, SCL_MISC);
    const char* key = clr_str(a[1]);
    if (!key){ osapi_set_last_error(ERR_BADARG); return 0; }
    return osapi_cfg_del_c((h_t)a[0], key) == ERR_OK ? 1 : 0;
}
int32_t osapi_ic_cfg_key_at(int32_t* a){
    ICALL_GUARD(CAP_CFG, SCL_MISC);
    static char out[CFG_KEY_MAX];
    int rc = osapi_cfg_keys_c((h_t)a[0], (int)a[1], out, sizeof(out));
    if (rc != ERR_OK) return 0;                  // null => end of keys
    return clr_new_str(out);
}
int32_t osapi_ic_cfg_flush(int32_t* a){
    ICALL_GUARD(CAP_CFG, SCL_MISC);
    return osapi_cfg_flush_c((h_t)a[0]) == ERR_OK ? 1 : 0;
}
int32_t osapi_ic_cfg_close(int32_t* a){
    ICALL_GUARD(CAP_CFG, SCL_MISC);
    return osapi_cfg_close_c((h_t)a[0]) == ERR_OK ? 1 : 0;
}
int32_t osapi_ic_cfg_count(int32_t* a){
    ICALL_GUARD(CAP_CFG, SCL_MISC);
    return osapi_cfg_count_c((h_t)a[0]);
}
int32_t osapi_ic_cfg_dirty(int32_t* a){
    ICALL_GUARD(CAP_CFG, SCL_MISC);
    return osapi_cfg_dirty_c((h_t)a[0]);
}

// ---- kernel-side self-test: Set -> Flush -> reopen -> Get ---------------
int32_t osapi_ic_cfg_self_test(int32_t*){
    int fails = 0;
    const char* NM = "cfg12t";
    nexos_mcp_fs_remove(0, "cfg12t.cfg");        // idempotent across boots
    nexos_mcp_fs_remove(0, "cfg12t.tmp");

    osapi_clear_last_error();
    if (osapi_cfg_open_c("../x") != 0 || osapi_get_last_error() != ERR_BADARG){
        fails++; oser("  cfg bad name accepted\n");
    }
    osapi_clear_last_error();
    if (osapi_cfg_open_c("") != 0 || osapi_get_last_error() != ERR_BADARG){
        fails++; oser("  cfg empty name accepted\n");
    }

    h_t h = osapi_cfg_open_c(NM);
    if (!h){ fails++; oser("  cfg open failed\n"); }
    else {
        char v[CFG_VAL_MAX];
        char kb[CFG_KEY_MAX];
        osapi_clear_last_error();
        if (osapi_cfg_get_c(h, "k", v, sizeof(v)) != ERR_NOENTRY ||
            osapi_get_last_error() != ERR_NOENTRY){
            fails++; oser("  cfg missing key not NoEntry\n");
        }
        if (osapi_cfg_set_c(h, "k", "v") != ERR_OK){ fails++; oser("  cfg set k\n"); }
        if (osapi_cfg_set_c(h, "n", "42") != ERR_OK){ fails++; oser("  cfg set n\n"); }
        if (osapi_cfg_get_c(h, "k", v, sizeof(v)) != ERR_OK || !ostr_eq(v, "v")){
            fails++; oser("  cfg get k wrong\n");
        }
        if (osapi_cfg_count_c(h) != 2){ fails++; oser("  cfg count != 2\n"); }
        if (osapi_cfg_keys_c(h, 0, kb, sizeof(kb)) != ERR_OK || !ostr_eq(kb, "k")){
            fails++; oser("  cfg key[0] wrong\n");
        }
        if (osapi_cfg_keys_c(h, 1, kb, sizeof(kb)) != ERR_OK || !ostr_eq(kb, "n")){
            fails++; oser("  cfg key[1] wrong\n");
        }
        if (osapi_cfg_keys_c(h, 2, kb, sizeof(kb)) != ERR_NOENTRY){
            fails++; oser("  cfg key[2] not NoEntry\n");
        }
        if (osapi_cfg_dirty_c(h) != 1){ fails++; oser("  cfg dirty not set\n"); }
        if (osapi_cfg_flush_c(h) != ERR_OK){ fails++; oser("  cfg flush failed\n"); }
        if (osapi_cfg_dirty_c(h) != 0){ fails++; oser("  cfg dirty not cleared\n"); }
        static unsigned char probe[1];
        if (nexos_mcp_fs_read(0, "cfg12t.cfg", probe, 1) <= 0){
            fails++; oser("  cfg file missing after flush\n");
        }
        if (nexos_mcp_fs_read(0, "cfg12t.tmp", probe, 1) > 0){
            fails++; oser("  cfg tmp leaked after rename\n");
        }
        // Reopen from disk: the persisted value must survive.
        h_t h2 = osapi_cfg_open_c(NM);
        if (!h2){ fails++; oser("  cfg reopen failed\n"); }
        else {
            if (osapi_cfg_get_c(h2, "k", v, sizeof(v)) != ERR_OK || !ostr_eq(v, "v")){
                fails++; oser("  cfg persisted k wrong\n");
            }
            if (osapi_cfg_get_c(h2, "n", v, sizeof(v)) != ERR_OK || !ostr_eq(v, "42")){
                fails++; oser("  cfg persisted n wrong\n");
            }
            if (osapi_cfg_close_c(h2) != ERR_OK){ fails++; oser("  cfg close h2\n"); }
        }
        // Overlong key / value must be rejected, not truncated.
        static char big[CFG_KEY_MAX + 8];
        for (int i = 0; i < CFG_KEY_MAX + 4; i++) big[i] = 'a';
        big[CFG_KEY_MAX + 4] = 0;
        if (osapi_cfg_set_c(h, big, "x") != ERR_TOOBIG){
            fails++; oser("  cfg overlong key accepted\n");
        }
        if (osapi_cfg_del_c(h, "n") != ERR_OK){ fails++; oser("  cfg del n\n"); }
        if (osapi_cfg_get_c(h, "n", v, sizeof(v)) != ERR_NOENTRY){
            fails++; oser("  cfg deleted key still readable\n");
        }
        if (osapi_cfg_close_c(h) != ERR_OK){ fails++; oser("  cfg close h\n"); }
        // Stale handle: close bumped the generation.
        osapi_clear_last_error();
        if (osapi_cfg_count_c(h) >= 0){ fails++; oser("  cfg stale handle resolved\n"); }
        if (osapi_get_last_error() != ERR_STALE &&
            osapi_get_last_error() != ERR_BADHANDLE){
            fails++; oser("  cfg stale err wrong\n");
        }
    }
    nexos_mcp_fs_remove(0, "cfg12t.cfg");
    nexos_mcp_fs_remove(0, "cfg12t.tmp");
    oser("[OSAPI] cfg self-test fails="); oser_dec(fails); oser("\n");
    return fails;
}

// =====================================================================
//  L4  memory: handle-only persistent heap / arena / shm / mapping ledger
// ---------------------------------------------------------------------
//  Freestanding: kmalloc()/kfree() are the 64-bit kernel's real persistent
//  heap (heap_init() runs at K64-7, long before mforms_init()).  Nothing
//  here touches clr_str/clr_new_str, so osapi_ic_mem_self_test() really
//  executes outside a managed context instead of silently skipping.
// =====================================================================
extern "C" void* kmalloc(uint32_t size);
extern "C" void  kfree(void* ptr);

static OsMemBlock g_mblocks[MEM_MAX_BLOCKS];
static OsShmSeg   g_shm[MEM_MAX_SHM];
static OsMemMap   g_maps[MEM_MAX_MAPS];
static uint32_t   g_mem_owned_bytes = 0;   // heap+arena bytes from kmalloc

static void mzero(void* p, uint32_t n){
    unsigned char* d = (unsigned char*)p;
    for (uint32_t i = 0; i < n; i++) d[i] = 0;
}
static void mcopy(void* d, const void* s, uint32_t n){
    unsigned char* dd = (unsigned char*)d;
    const unsigned char* ss = (const unsigned char*)s;
    for (uint32_t i = 0; i < n; i++) dd[i] = ss[i];
}
static int mname_ok(const char* s){
    if (!s || !s[0]) return 0;
    int n = 0;
    while (s[n]){
        char c = s[n];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) return 0;
        n++;
        if (n >= MEM_NAME_MAX) return 0;
    }
    return 1;
}
static int mblock_free_slot(void){
    for (int i = 0; i < MEM_MAX_BLOCKS; i++)
        if (!g_mblocks[i].used) return i;
    osapi_set_last_error(ERR_NOSPACE);
    return -1;
}

// Resolve a memory handle.  Two independent generation checks:
//   osapi_handle_resolve() -> global handle-table gen (bumped by close)
//   b->handle_gen          -> this slot's own record of the live handle
static OsMemBlock* mem_from(h_t h, uint8_t kind){
    if (h == 0){ osapi_set_last_error(ERR_BADHANDLE); return 0; }
    void* p = osapi_handle_resolve(h, kind, g_active_token);
    if (!p) return 0;                       // error already set by resolve
    OsMemBlock* b = (OsMemBlock*)p;
    if (!b->used || (kind != 0xFF && b->kind != kind)){
        osapi_set_last_error(ERR_BADHANDLE); return 0;
    }
    if (b->handle_gen != hgen(h)){
        osapi_set_last_error(ERR_STALE); return 0;
    }
    if (b->owner != g_active_token){
        osapi_set_last_error(ERR_PERMDENIED); return 0;
    }
    return b;
}
static h_t mem_bind(OsMemBlock* b, int slot, uint8_t kind){
    h_t h = osapi_handle_alloc(kind, (uint16_t)slot, g_active_token, b,
                               HR_R | HR_W);
    if (!h) return 0;                       // ERR_NOSPACE already set
    b->handle_gen = hgen(h);
    osapi_set_last_error(ERR_OK);
    return h;
}
// Close the handle first (gen++), then advance the slot's own generation.
// Both must happen on every free path -- heap, arena and shm alias.
static int mem_unbind(h_t h, OsMemBlock* b){
    int rc = osapi_handle_close(h, g_active_token);
    b->gen++;
    if (b->gen == 0) b->gen = 1;
    b->handle_gen = 0;
    if (b->base && b->kind != HK_SHM){
        kfree(b->base);
        g_mem_owned_bytes -= b->size;
    }
    b->used = 0; b->kind = 0; b->owner = 0; b->size = 0;
    b->used_bytes = 0; b->base = 0; b->backing = 0; b->name[0] = 0;
    return rc;
}

// ---- heap ------------------------------------------------------------
h_t osapi_heap_alloc_c(uint32_t sz){
    if (sz == 0){ osapi_set_last_error(ERR_BADARG); return 0; }
    if (sz > MEM_BLOCK_CAP){ osapi_set_last_error(ERR_TOOBIG); return 0; }
    int i = mblock_free_slot();
    if (i < 0) return 0;
    void* p = kmalloc(sz);
    if (!p){ osapi_set_last_error(ERR_NOMEM); return 0; }
    mzero(p, sz);
    OsMemBlock* b = &g_mblocks[i];
    b->used = 1; b->kind = HK_HEAP;
    b->gen++; if (b->gen == 0) b->gen = 1;
    b->owner = g_active_token; b->size = sz; b->used_bytes = sz;
    b->base = p; b->backing = 0; b->name[0] = 0; b->handle_gen = 0;
    h_t h = mem_bind(b, i, HK_HEAP);
    if (!h){ kfree(p); b->used = 0; b->base = 0; return 0; }
    g_mem_owned_bytes += sz;
    return h;
}
int osapi_heap_free_c(h_t h){
    OsMemBlock* b = mem_from(h, HK_HEAP);
    if (!b) return 0;
    return mem_unbind(h, b);
}
h_t osapi_heap_realloc_c(h_t h, uint32_t sz){
    if (sz == 0){ osapi_set_last_error(ERR_BADARG); return 0; }
    if (sz > MEM_BLOCK_CAP){ osapi_set_last_error(ERR_TOOBIG); return 0; }
    OsMemBlock* b = mem_from(h, HK_HEAP);
    if (!b) return 0;
    int slot = (int)(b - g_mblocks);
    void* np = kmalloc(sz);
    if (!np){ osapi_set_last_error(ERR_NOMEM); return 0; }
    mzero(np, sz);
    uint32_t cp = b->size < sz ? b->size : sz;
    mcopy(np, b->base, cp);
    void* op = b->base;
    uint32_t osz = b->size;
    // Invalidate the OLD handle BEFORE issuing the new one.  The table gen
    // and the slot gen both advance, so the old h_t can never come back.
    if (!osapi_handle_close(h, g_active_token)){
        kfree(np); osapi_set_last_error(ERR_BADHANDLE); return 0;
    }
    b->gen++; if (b->gen == 0) b->gen = 1;
    b->handle_gen = 0;
    kfree(op);
    g_mem_owned_bytes -= osz;
    b->base = np; b->size = sz; b->used_bytes = sz;
    h_t nh = mem_bind(b, slot, HK_HEAP);
    if (!nh){ kfree(np); b->used = 0; b->base = 0; return 0; }
    g_mem_owned_bytes += sz;
    return nh;
}
int osapi_heap_size_c(h_t h){
    OsMemBlock* b = mem_from(h, HK_HEAP);
    if (!b) return -osapi_get_last_error();
    osapi_set_last_error(ERR_OK);
    return (int)b->size;
}
int osapi_heap_write_c(h_t h, uint32_t off, const unsigned char* p, int n){
    OsMemBlock* b = mem_from(h, HK_HEAP);
    if (!b) return 0;
    if (n < 0 || !p || off > b->size || (uint32_t)n > b->size - off){
        osapi_set_last_error(ERR_BADARG); return 0;
    }
    mcopy((unsigned char*)b->base + off, p, (uint32_t)n);
    osapi_set_last_error(ERR_OK);
    return 1;
}
int osapi_heap_read_c(h_t h, uint32_t off, unsigned char* p, int n){
    OsMemBlock* b = mem_from(h, HK_HEAP);
    if (!b) return 0;
    if (n < 0 || !p || off > b->size || (uint32_t)n > b->size - off){
        osapi_set_last_error(ERR_BADARG); return 0;
    }
    mcopy(p, (unsigned char*)b->base + off, (uint32_t)n);
    osapi_set_last_error(ERR_OK);
    return 1;
}
void* osapi_mem_ptr_c(h_t h){
    OsMemBlock* b = mem_from(h, 0xFF);
    if (!b) return 0;
    if (b->kind == HK_SHM){
        if (b->backing >= MEM_MAX_SHM || !g_shm[b->backing].used){
            osapi_set_last_error(ERR_BROKEN); return 0;
        }
        return g_shm[b->backing].base;
    }
    return b->base;
}
int osapi_mem_slot_gen_c(int slot){
    if (slot < 0 || slot >= MEM_MAX_BLOCKS){
        osapi_set_last_error(ERR_BADARG); return -1;
    }
    return (int)g_mblocks[slot].gen;
}
int osapi_mem_live_c(void){
    int n = 0;
    for (int i = 0; i < MEM_MAX_BLOCKS; i++) if (g_mblocks[i].used) n++;
    return n;
}

// ---- arena (bump allocator with mark/release rollback) ---------------
h_t osapi_arena_new_c(uint32_t cap){
    if (cap == 0){ osapi_set_last_error(ERR_BADARG); return 0; }
    if (cap > MEM_BLOCK_CAP){ osapi_set_last_error(ERR_TOOBIG); return 0; }
    int i = mblock_free_slot();
    if (i < 0) return 0;
    void* p = kmalloc(cap);
    if (!p){ osapi_set_last_error(ERR_NOMEM); return 0; }
    mzero(p, cap);
    OsMemBlock* b = &g_mblocks[i];
    b->used = 1; b->kind = HK_ARENA;
    b->gen++; if (b->gen == 0) b->gen = 1;
    b->owner = g_active_token; b->size = cap; b->used_bytes = 0;
    b->base = p; b->backing = 0; b->name[0] = 0; b->handle_gen = 0;
    h_t h = mem_bind(b, i, HK_ARENA);
    if (!h){ kfree(p); b->used = 0; b->base = 0; return 0; }
    g_mem_owned_bytes += cap;
    return h;
}
int osapi_arena_alloc_c(h_t h, uint32_t sz){
    OsMemBlock* b = mem_from(h, HK_ARENA);
    if (!b) return -osapi_get_last_error();
    if (sz == 0){ osapi_set_last_error(ERR_BADARG); return -ERR_BADARG; }
    if (sz > b->size - b->used_bytes){
        osapi_set_last_error(ERR_NOSPACE); return -ERR_NOSPACE;
    }
    int off = (int)b->used_bytes;
    mzero((unsigned char*)b->base + off, sz);
    b->used_bytes += sz;
    osapi_set_last_error(ERR_OK);
    return off;                 // 0 is a VALID offset; <0 means failure
}
int osapi_arena_mark_c(h_t h){
    OsMemBlock* b = mem_from(h, HK_ARENA);
    if (!b) return -osapi_get_last_error();
    osapi_set_last_error(ERR_OK);
    return (int)b->used_bytes;
}
int osapi_arena_release_c(h_t h, int mark){
    OsMemBlock* b = mem_from(h, HK_ARENA);
    if (!b) return 0;
    if (mark < 0 || (uint32_t)mark > b->used_bytes){
        osapi_set_last_error(ERR_BADARG); return 0;
    }
    b->used_bytes = (uint32_t)mark;
    osapi_set_last_error(ERR_OK);
    return 1;
}
int osapi_arena_used_c(h_t h){
    return osapi_arena_mark_c(h);
}
int osapi_arena_free_c(h_t h){
    OsMemBlock* b = mem_from(h, HK_ARENA);
    if (!b) return 0;
    return mem_unbind(h, b);
}

// ---- shared memory: named segments, per-open handles -----------------
// Each open gets its OWN slot (handle-table slots must be unique per kind)
// but points at the same OsShmSeg.  The segment dies only when refs hits 0.
static int shm_find(const char* name){
    for (int i = 0; i < MEM_MAX_SHM; i++)
        if (g_shm[i].used && ostr_eq(g_shm[i].name, name)) return i;
    return -1;
}
static h_t shm_handle_for(int seg, const char* name){
    int i = mblock_free_slot();
    if (i < 0) return 0;
    OsMemBlock* b = &g_mblocks[i];
    b->used = 1; b->kind = HK_SHM;
    b->gen++; if (b->gen == 0) b->gen = 1;
    b->owner = g_active_token;
    b->size = g_shm[seg].size; b->used_bytes = g_shm[seg].size;
    b->base = 0; b->backing = (uint16_t)seg;
    ostr_cpy(b->name, name, MEM_NAME_MAX);
    b->handle_gen = 0;
    h_t h = mem_bind(b, i, HK_SHM);
    if (!h){ b->used = 0; b->base = 0; return 0; }
    return h;
}
h_t osapi_shm_create_c(const char* name, uint32_t sz){
    if (!mname_ok(name)){ osapi_set_last_error(ERR_BADARG); return 0; }
    if (sz == 0){ osapi_set_last_error(ERR_BADARG); return 0; }
    if (sz > MEM_BLOCK_CAP){ osapi_set_last_error(ERR_TOOBIG); return 0; }
    if (shm_find(name) >= 0){ osapi_set_last_error(ERR_NAMEEXISTS); return 0; }
    int seg = -1;
    for (int i = 0; i < MEM_MAX_SHM; i++) if (!g_shm[i].used){ seg = i; break; }
    if (seg < 0){ osapi_set_last_error(ERR_NOSPACE); return 0; }
    void* p = kmalloc(sz);
    if (!p){ osapi_set_last_error(ERR_NOMEM); return 0; }
    mzero(p, sz);
    g_shm[seg].used = 1; g_shm[seg].base = p; g_shm[seg].size = sz;
    g_shm[seg].refs = 1;
    ostr_cpy(g_shm[seg].name, name, MEM_NAME_MAX);
    h_t h = shm_handle_for(seg, name);
    if (!h){
        kfree(p); g_shm[seg].used = 0; g_shm[seg].base = 0;
        g_shm[seg].refs = 0;
        return 0;
    }
    return h;
}
h_t osapi_shm_open_c(const char* name){
    if (!mname_ok(name)){ osapi_set_last_error(ERR_BADARG); return 0; }
    int seg = shm_find(name);
    if (seg < 0){ osapi_set_last_error(ERR_NOTFOUND); return 0; }
    g_shm[seg].refs++;
    h_t h = shm_handle_for(seg, name);
    if (!h){ g_shm[seg].refs--; return 0; }
    return h;
}
int osapi_shm_size_c(h_t h){
    OsMemBlock* b = mem_from(h, HK_SHM);
    if (!b) return -osapi_get_last_error();
    int seg = (int)b->backing;
    if (seg >= MEM_MAX_SHM || !g_shm[seg].used){
        osapi_set_last_error(ERR_BROKEN); return -ERR_BROKEN;
    }
    osapi_set_last_error(ERR_OK);
    return (int)g_shm[seg].size;
}
int osapi_shm_close_c(h_t h){
    OsMemBlock* b = mem_from(h, HK_SHM);
    if (!b) return 0;
    int seg = (int)b->backing;
    int rc = osapi_handle_close(h, g_active_token);
    b->gen++; if (b->gen == 0) b->gen = 1;
    b->handle_gen = 0;
    b->used = 0; b->kind = 0; b->owner = 0; b->base = 0;
    b->backing = 0; b->name[0] = 0; b->size = 0; b->used_bytes = 0;
    if (rc && seg < MEM_MAX_SHM && g_shm[seg].used){
        if (g_shm[seg].refs > 0) g_shm[seg].refs--;
        if (g_shm[seg].refs == 0){
            kfree(g_shm[seg].base);
            g_shm[seg].used = 0; g_shm[seg].base = 0;
            g_shm[seg].size = 0; g_shm[seg].name[0] = 0;
        }
    }
    return rc;
}

// ---- mapping ledger (MMIO / framebuffer accounting only) -------------
// No page-table change is performed: the 64-bit kernel is identity-mapped.
// What this layer guarantees is that a mapping is recorded, bounded,
// non-overlapping, and gated by dev_class_mask.
int osapi_mmap_c(int what, uint32_t addr, uint32_t len){
    if (len == 0){ osapi_set_last_error(ERR_BADARG); return 0; }
    if (len > (1u << 24)){ osapi_set_last_error(ERR_TOOBIG); return 0; }
    int cls;
    if (what == MM_WHAT_FB)        cls = MM_DEV_FB;
    else if (what == MM_WHAT_MMIO) cls = MM_DEV_MMIO;
    else { osapi_set_last_error(ERR_BADARG); return 0; }
    if (!osapi_dev_allowed(g_active_token, cls, (int)(addr >> 16))) return 0;
    uint64_t a0 = addr, a1 = addr + (uint64_t)len;
    for (int i = 0; i < MEM_MAX_MAPS; i++){
        if (!g_maps[i].used || g_maps[i].owner != g_active_token) continue;
        uint64_t b0 = g_maps[i].addr, b1 = b0 + g_maps[i].len;
        if (a0 < b1 && b0 < a1){ osapi_set_last_error(ERR_BUSY); return 0; }
    }
    int slot = -1;
    for (int i = 0; i < MEM_MAX_MAPS; i++) if (!g_maps[i].used){ slot = i; break; }
    if (slot < 0){ osapi_set_last_error(ERR_NOSPACE); return 0; }
    g_maps[slot].used = 1; g_maps[slot].what = (uint8_t)what;
    g_maps[slot].addr = addr; g_maps[slot].len = len;
    g_maps[slot].owner = g_active_token;
    osapi_set_last_error(ERR_OK);
    return slot + 1;
}
int osapi_munmap_c(int id){
    int i = id - 1;
    if (i < 0 || i >= MEM_MAX_MAPS || !g_maps[i].used){
        osapi_set_last_error(ERR_NOTFOUND); return 0;
    }
    if (g_maps[i].owner != g_active_token){
        osapi_set_last_error(ERR_PERMDENIED); return 0;
    }
    g_maps[i].used = 0;
    osapi_set_last_error(ERR_OK);
    return 1;
}
int osapi_mmap_count_c(void){
    int n = 0;
    for (int i = 0; i < MEM_MAX_MAPS; i++)
        if (g_maps[i].used && g_maps[i].owner == g_active_token) n++;
    return n;
}

// ---- icall wrappers ---------------------------------------------------
// GUARD on every entry.  GUARD's `return 0` is fine for all of these:
// 0 is the invalid handle / failure result for handle-producing calls,
// and the read-only counters legitimately report 0 when denied.
int32_t osapi_ic_mem_heap_alloc(int32_t* a){
    ICALL_GUARD(CAP_MEM, SCL_MEM);
    return (int32_t)osapi_heap_alloc_c((uint32_t)a[0]);
}
int32_t osapi_ic_mem_heap_free(int32_t* a){
    ICALL_GUARD(CAP_MEM, SCL_MEM);
    return (int32_t)osapi_heap_free_c((h_t)a[0]);
}
int32_t osapi_ic_mem_heap_realloc(int32_t* a){
    ICALL_GUARD(CAP_MEM, SCL_MEM);
    return (int32_t)osapi_heap_realloc_c((h_t)a[0], (uint32_t)a[1]);
}
int32_t osapi_ic_mem_heap_size(int32_t* a){
    ICALL_GUARD(CAP_MEM, SCL_MEM);
    int v = osapi_heap_size_c((h_t)a[0]);
    return v > 0 ? v : 0;
}
int32_t osapi_ic_mem_heap_poke(int32_t* a){
    ICALL_GUARD(CAP_MEM, SCL_MEM);
    unsigned char b = (unsigned char)(a[2] & 0xFF);
    return (int32_t)osapi_heap_write_c((h_t)a[0], (uint32_t)a[1], &b, 1);
}
int32_t osapi_ic_mem_heap_peek(int32_t* a){
    ICALL_GUARD(CAP_MEM, SCL_MEM);
    unsigned char b = 0;
    if (!osapi_heap_read_c((h_t)a[0], (uint32_t)a[1], &b, 1)) return -1;
    return (int32_t)b;
}
int32_t osapi_ic_mem_arena_new(int32_t* a){
    ICALL_GUARD(CAP_MEM, SCL_MEM);
    return (int32_t)osapi_arena_new_c((uint32_t)a[0]);
}
int32_t osapi_ic_mem_arena_alloc(int32_t* a){
    ICALL_GUARD(CAP_MEM, SCL_MEM);
    int v = osapi_arena_alloc_c((h_t)a[0], (uint32_t)a[1]);
    return v >= 0 ? v : -1;          // 0 is a valid offset
}
int32_t osapi_ic_mem_arena_mark(int32_t* a){
    ICALL_GUARD(CAP_MEM, SCL_MEM);
    int v = osapi_arena_mark_c((h_t)a[0]);
    return v >= 0 ? v : -1;
}
int32_t osapi_ic_mem_arena_release(int32_t* a){
    ICALL_GUARD(CAP_MEM, SCL_MEM);
    return (int32_t)osapi_arena_release_c((h_t)a[0], (int)a[1]);
}
int32_t osapi_ic_mem_arena_used(int32_t* a){
    ICALL_GUARD(CAP_MEM, SCL_MEM);
    int v = osapi_arena_used_c((h_t)a[0]);
    return v >= 0 ? v : -1;
}
int32_t osapi_ic_mem_arena_free(int32_t* a){
    ICALL_GUARD(CAP_MEM, SCL_MEM);
    return (int32_t)osapi_arena_free_c((h_t)a[0]);
}
int32_t osapi_ic_mem_shm_create(int32_t* a){
    ICALL_GUARD(CAP_MEM, SCL_MEM);
    const char* n = clr_str(a[0]);
    if (!n){ osapi_set_last_error(ERR_BADARG); return 0; }
    return (int32_t)osapi_shm_create_c(n, (uint32_t)a[1]);
}
int32_t osapi_ic_mem_shm_open(int32_t* a){
    ICALL_GUARD(CAP_MEM, SCL_MEM);
    const char* n = clr_str(a[0]);
    if (!n){ osapi_set_last_error(ERR_BADARG); return 0; }
    return (int32_t)osapi_shm_open_c(n);
}
int32_t osapi_ic_mem_shm_size(int32_t* a){
    ICALL_GUARD(CAP_MEM, SCL_MEM);
    int v = osapi_shm_size_c((h_t)a[0]);
    return v > 0 ? v : 0;
}
int32_t osapi_ic_mem_shm_close(int32_t* a){
    ICALL_GUARD(CAP_MEM, SCL_MEM);
    return (int32_t)osapi_shm_close_c((h_t)a[0]);
}
int32_t osapi_ic_mem_mmap(int32_t* a){
    ICALL_GUARD(CAP_MEM, SCL_MEM);
    return (int32_t)osapi_mmap_c((int)a[0], (uint32_t)a[1], (uint32_t)a[2]);
}
int32_t osapi_ic_mem_unmap(int32_t* a){
    ICALL_GUARD(CAP_MEM, SCL_MEM);
    return (int32_t)osapi_munmap_c((int)a[0]);
}
int32_t osapi_ic_mem_mappings(int32_t*){
    ICALL_GUARD(CAP_MEM, SCL_MEM);
    return (int32_t)osapi_mmap_count_c();
}
int32_t osapi_ic_mem_live(int32_t*){
    ICALL_GUARD(CAP_MEM, SCL_MEM);
    return (int32_t)osapi_mem_live_c();
}

// =====================================================================
//  L4 kernel-side self-test -- PURE C, no clr_str / clr_new_str anywhere.
//  The gen++ anti-dangling guarantee is asserted with actual numbers:
//    * slot gen must advance on HeapFree
//    * a same-slot reallocation must carry a DIFFERENT handle gen
//    * the old h_t must then fail with ERR_STALE (not merely "not found")
//    * closed handles must also be rejected before any reuse
// =====================================================================
static void mem_test_drop_shm(const char* nm){
    int i = shm_find(nm);
    if (i < 0) return;
    if (g_shm[i].base) kfree(g_shm[i].base);
    g_shm[i].used = 0; g_shm[i].base = 0; g_shm[i].size = 0;
    g_shm[i].refs = 0; g_shm[i].name[0] = 0;
}
static uint32_t s_guard_tok = 0;

int32_t osapi_ic_mem_self_test(int32_t*){
    int fails = 0;
    const char* SN = "m4t";
    oser("[OSAPI] mem self-test begin\n");
    mem_test_drop_shm(SN);                 // idempotent across repeated runs
    int live0 = osapi_mem_live_c();
    int maps0 = osapi_mmap_count_c();
    unsigned char pat[4] = { 0x5A, 0xA5, 0x11, 0x22 };
    unsigned char got[4];

    // ---- 1. heap lifecycle + bounds ----------------------------------
    h_t h1 = osapi_heap_alloc_c(64);
    if (!h1){ fails++; oser("  mem heap alloc failed\n"); }
    else {
        if (osapi_heap_size_c(h1) != 64){ fails++; oser("  mem heap size != 64\n"); }
        if (osapi_heap_write_c(h1, 0, pat, 4) != 1){ fails++; oser("  mem heap write\n"); }
        for (int i = 0; i < 4; i++) got[i] = 0;
        if (osapi_heap_read_c(h1, 0, got, 4) != 1 || got[0] != 0x5A ||
            got[1] != 0xA5 || got[2] != 0x11 || got[3] != 0x22){
            fails++; oser("  mem heap read back wrong\n");
        }
        osapi_clear_last_error();
        if (osapi_heap_write_c(h1, 62, pat, 4) != 0 ||
            osapi_get_last_error() != ERR_BADARG){
            fails++; oser("  mem heap out-of-bounds write accepted\n");
        }
        void* rp = osapi_mem_ptr_c(h1);
        if (!rp){ fails++; oser("  mem ring0 ptr null\n"); }

        // ---- 2. THE gen++ PROOF -------------------------------------
        int slot = (int)hslot(h1);
        int g0   = osapi_mem_slot_gen_c(slot);
        int hg0  = (int)hgen(h1);
        if (osapi_heap_free_c(h1) != 1){ fails++; oser("  mem heap free failed\n"); }
        int g1 = osapi_mem_slot_gen_c(slot);
        if (g1 != g0 + 1){ fails++; oser("  mem slot gen NOT bumped on free\n"); }
        osapi_clear_last_error();
        if (osapi_heap_free_c(h1) != 0){
            fails++; oser("  mem double free succeeded\n");
        }
        if (osapi_get_last_error() != ERR_BADHANDLE &&
            osapi_get_last_error() != ERR_STALE){
            fails++; oser("  mem double free err wrong\n");
        }
        // Reallocate the SAME slot, then demand the stale h1 be STALE.
        h_t h2 = osapi_heap_alloc_c(64);
        int h2live = 0;
        if (!h2){ fails++; oser("  mem heap re-alloc failed\n"); }
        else {
            h2live = 1;
            int hg2 = (int)hgen(h2);
            if ((int)hslot(h2) != slot){ fails++; oser("  mem re-alloc used other slot\n"); }
            if (hg2 == hg0){ fails++; oser("  mem handle gen NOT bumped\n"); }
            osapi_clear_last_error();
            int v = osapi_heap_size_c(h1);
            if (v >= 0){ fails++; oser("  mem STALE handle RESOLVED after slot reuse\n"); }
            int stale_err = osapi_get_last_error();
            if (stale_err != ERR_STALE){ fails++; oser("  mem stale err is not STALE\n"); }
            oser("[OSAPI] L4 gen++ proof: slot="); oser_dec(slot);
            oser(" slotgen "); oser_dec(g0); oser("->"); oser_dec(g1);
            oser(" handlegen "); oser_dec(hg0); oser("->"); oser_dec(hg2);
            oser(" stale="); oser_dec(stale_err); oser("\n");

            // ---- 3. realloc: data preserved, old handle dead ---------
            if (osapi_heap_write_c(h2, 0, pat, 4) != 1){
                fails++; oser("  mem write before realloc\n");
            }
            h_t h3 = osapi_heap_realloc_c(h2, 128);
            if (!h3){ fails++; oser("  mem realloc failed\n"); }
            else {
                h2live = 0;
                if (osapi_heap_size_c(h3) != 128){ fails++; oser("  mem realloc size\n"); }
                for (int i = 0; i < 4; i++) got[i] = 0;
                if (osapi_heap_read_c(h3, 0, got, 4) != 1 || got[0] != 0x5A ||
                    got[3] != 0x22){ fails++; oser("  mem realloc lost data\n"); }
                osapi_clear_last_error();
                if (osapi_heap_size_c(h2) >= 0){
                    fails++; oser("  mem realloc old handle resolved\n");
                }
                if (osapi_heap_free_c(h3) != 1){ fails++; oser("  mem free h3\n"); }
            }
            if (h2live) osapi_heap_free_c(h2);
        }
    }

    // ---- 4. arena mark/release ---------------------------------------
    h_t ar = osapi_arena_new_c(256);
    if (!ar){ fails++; oser("  mem arena new failed\n"); }
    else {
        if (osapi_arena_used_c(ar) != 0){ fails++; oser("  mem arena initial used\n"); }
        if (osapi_arena_alloc_c(ar, 32) != 0){ fails++; oser("  mem arena alloc1 off\n"); }
        if (osapi_arena_alloc_c(ar, 64) != 32){ fails++; oser("  mem arena alloc2 off\n"); }
        if (osapi_arena_mark_c(ar) != 96){ fails++; oser("  mem arena mark != 96\n"); }
        osapi_clear_last_error();
        if (osapi_arena_release_c(ar, 999) != 0 ||
            osapi_get_last_error() != ERR_BADARG){
            fails++; oser("  mem arena bad mark accepted\n");
        }
        if (osapi_arena_release_c(ar, 33) != 1){ fails++; oser("  mem arena release\n"); }
        if (osapi_arena_used_c(ar) != 33){ fails++; oser("  mem arena used != 33\n"); }
        if (osapi_arena_alloc_c(ar, 16) != 33){ fails++; oser("  mem arena re-alloc off\n"); }
        osapi_clear_last_error();
        if (osapi_arena_alloc_c(ar, 1024) >= 0 ||
            osapi_get_last_error() != ERR_NOSPACE){
            fails++; oser("  mem arena overflow accepted\n");
        }
        if (osapi_arena_free_c(ar) != 1){ fails++; oser("  mem arena free\n"); }
        osapi_clear_last_error();
        if (osapi_arena_mark_c(ar) >= 0){
            fails++; oser("  mem arena stale handle resolved\n");
        }
    }

    // ---- 5. shm create/open/refs/last-close --------------------------
    h_t s1 = osapi_shm_create_c(SN, 128);
    h_t s2 = 0;
    if (!s1){ fails++; oser("  mem shm create failed\n"); }
    else {
        osapi_clear_last_error();
        if (osapi_shm_create_c(SN, 128) != 0 ||
            osapi_get_last_error() != ERR_NAMEEXISTS){
            fails++; oser("  mem shm duplicate create accepted\n");
        }
        if (osapi_shm_size_c(s1) != 128){ fails++; oser("  mem shm size\n"); }
        s2 = osapi_shm_open_c(SN);
        if (!s2){ fails++; oser("  mem shm open failed\n"); }
        if (osapi_shm_size_c(s2) != 128){ fails++; oser("  mem shm alias size\n"); }
        if (osapi_mem_ptr_c(s2) != osapi_mem_ptr_c(s1)){
            fails++; oser("  mem shm aliases differ\n");
        }
        if (osapi_shm_close_c(s1) != 1){ fails++; oser("  mem shm close s1\n"); }
        if (osapi_shm_size_c(s2) != 128){
            fails++; oser("  mem shm died with first handle\n");
        }
        osapi_clear_last_error();
        if (osapi_shm_size_c(s1) >= 0){
            fails++; oser("  mem shm stale handle resolved\n");
        }
    }
    if (s2 && osapi_shm_close_c(s2) != 1){ fails++; oser("  mem shm close s2\n"); }
    osapi_clear_last_error();
    if (osapi_shm_open_c(SN) != 0 || osapi_get_last_error() != ERR_NOTFOUND){
        fails++; oser("  mem shm segment leaked after last close\n");
    }

    // ---- 6. mapping ledger + dev_class GUARD -------------------------
    int id = osapi_mmap_c(MM_WHAT_FB, 0xE0000000u, 4096);
    if (id <= 0){ fails++; oser("  mem mmap fb failed\n"); }
    if (osapi_mmap_count_c() != maps0 + 1){ fails++; oser("  mem mmap count\n"); }
    osapi_clear_last_error();
    if (osapi_mmap_c(MM_WHAT_FB, 0xE0000000u, 4096) > 0 ||
        osapi_get_last_error() != ERR_BUSY){
        fails++; oser("  mem overlapping mmap accepted\n");
    }
    osapi_clear_last_error();
    if (osapi_mmap_c(9, 0x1000, 16) > 0 || osapi_get_last_error() != ERR_BADARG){
        fails++; oser("  mem bad mmap what accepted\n");
    }
    OsBoundary* bd = osapi_boundary_of(g_active_token);
    if (!bd){ fails++; oser("  mem boundary missing\n"); }
    else {
        uint32_t save = bd->dev_class_mask;
        bd->dev_class_mask &= ~(1u << MM_DEV_MMIO);
        osapi_clear_last_error();
        if (osapi_mmap_c(MM_WHAT_MMIO, 0xF0000000u, 64) > 0 ||
            osapi_get_last_error() != ERR_PERMDENIED){
            fails++; oser("  mem MMIO dev guard not enforced\n");
        }
        bd->dev_class_mask = save;
    }
    if (id > 0 && osapi_munmap_c(id) != 1){ fails++; oser("  mem unmap\n"); }
    if (osapi_mmap_count_c() != maps0){ fails++; oser("  mem mappings leaked\n"); }
    osapi_clear_last_error();
    if (id > 0 && (osapi_munmap_c(id) != 0 ||
        osapi_get_last_error() != ERR_NOTFOUND)){
        fails++; oser("  mem double unmap accepted\n");
    }

    // ---- 7. ICALL_GUARD: both gates really refuse -------------------
    // A downgraded child token is built once and reused so repeated runs
    // of this self-test stay idempotent.
    uint32_t saved_tok = g_active_token;
    if (!s_guard_tok){
        OsBoundary tmp; mzero(&tmp, (uint32_t)sizeof(tmp));
        OsBoundary* cur = osapi_boundary_of(saved_tok);
        if (cur) mcopy(&tmp, cur, (uint32_t)sizeof(tmp));
        s_guard_tok = osapi_token_new(77, saved_tok, &tmp);
    }
    OsToken* ct = s_guard_tok ? osapi_token_get(s_guard_tok) : 0;
    if (!ct){ fails++; oser("  mem guard token missing\n"); }
    else {
        uint32_t cs = ct->caps_mask;
        uint64_t ss = ct->syscall_mask;
        int32_t ga[1]; ga[0] = 64;
        ct->caps_mask &= ~(1u << CAP_MEM);
        ct->syscall_mask &= ~(1ull << SCL_MEM);
        osapi_set_cur_token(s_guard_tok);
        osapi_clear_last_error();
        if (osapi_ic_mem_heap_alloc(ga) != 0 ||
            osapi_get_last_error() != ERR_PERMDENIED){
            fails++; oser("  mem GUARD syscall gate not enforced\n");
        }
        ct->syscall_mask = ss;         // restore syscall, cap still withdrawn
        osapi_clear_last_error();
        if (osapi_ic_mem_heap_alloc(ga) != 0 ||
            osapi_get_last_error() != ERR_PERMDENIED){
            fails++; oser("  mem GUARD cap gate not enforced\n");
        }
        ct->caps_mask = cs; ct->syscall_mask = ss;
        osapi_set_cur_token(saved_tok);
    }

    // ---- 8. no leaks -------------------------------------------------
    if (osapi_mem_live_c() != live0){ fails++; oser("  mem blocks leaked\n"); }
    if (osapi_mmap_count_c() != maps0){ fails++; oser("  mem maps leaked\n"); }
    (void)g_mem_owned_bytes;
    oser("[OSAPI] mem self-test fails="); oser_dec(fails); oser("\n");
    return fails;
}

// =====================================================================
//  AI-assisted compatibility layer (D-class) icalls
// ---------------------------------------------------------------------
//  These are the MANAGED-side entry points.  The external AI agent uses
//  the very same functions, but reaches them through the file protocol on
//  the MKFS data volume (ai_cmd.bin), not through the CLR.
// =====================================================================
#include "nexos_compat.h"

int32_t osapi_ic_compat_patch(int32_t* a){
    if (!a){ osapi_set_last_error(ERR_BADARG); return 0; }
    const char* dll = clr_str(a[0]);
    const char* fn  = clr_str(a[1]);
    if (!dll || !fn){ osapi_set_last_error(ERR_BADARG); return 0; }
    int action = (int)a[2];
    int sel    = (int)a[3];
    int flags  = (int)a[4];
    int r = compat_patch_iat(dll, fn, action, sel, (flags & 1) ? 1 : 0);
    if (!r) osapi_set_last_error(ERR_NOTSUPP);
    else    osapi_set_last_error(ERR_OK);
    return r;
}

int32_t osapi_ic_compat_status(int32_t* a){
    (void)a;
    // Pack the interesting counters into one int: slot count, live patches,
    // active PE sessions.
    return (int32_t)(compat_iat_count() |
                      (compat_patch_count() << 10) |
                      (compat_pe_active() << 20));
}

int32_t osapi_ic_compat_describe(int32_t* a){
    if (!a){ osapi_set_last_error(ERR_BADARG); return 0; }
    int idx = (int)a[0];
    const char* name = compat_iat_name(idx);
    if (!name || !name[0]){ osapi_set_last_error(ERR_NOENTRY); return 0; }
    char out[128];
    int n = 0;
    const char* p = name;
    while (p[n] && n < 100) n++;
    for (int i = 0; i < n && i < 100; i++) out[i] = p[i];
    out[n] = 0;
    osapi_set_last_error(ERR_OK);
    return clr_new_str(out);
}

int32_t osapi_ic_compat_observe(int32_t* a){
    if (!a){ osapi_set_last_error(ERR_BADARG); return 0; }
    compat_observe_set(a[0] ? 1 : 0);
    osapi_set_last_error(ERR_OK);
    return 1;
}

int32_t osapi_ic_compat_summary(int32_t* a){
    if (!a){ osapi_set_last_error(ERR_BADARG); return 0; }
    int32_t* dst = clr_arr_data(a[0]);
    int alen = clr_arr_len(a[0]);
    if (!dst || alen < 32){ osapi_set_last_error(ERR_BADARG); return 0; }
    compat_summary();
    // The summary is regenerated inside compat_summary(); expose it as text
    // so the managed shell (and the host AI) can read it without decoding
    // the binary event format.
    extern void compat_summary_text(char* out, int cap);
    compat_summary_text((char*)dst, alen);
    osapi_set_last_error(ERR_OK);
    return 1;
}

int32_t osapi_ic_compat_fault(int32_t* a){
    if (!a){ osapi_set_last_error(ERR_BADARG); return 0; }
    int32_t* dst = clr_arr_data(a[0]);
    int alen = clr_arr_len(a[0]);
    if (!dst || alen < 32){ osapi_set_last_error(ERR_BADARG); return 0; }
    extern void compat_fault_text(char* out, int cap);
    compat_fault_text((char*)dst, alen);
    osapi_set_last_error(ERR_OK);
    return 1;
}

// ---- L2/L3 process + glob self-test ---------------------------------
int32_t osapi_ic_proc_self_test(int32_t* a){
    (void)a;
    int fails = 0;
    oser("[OSAPI] proc/glob self-test\n");
    // 1) glob over a file we create right here (listing content is
    // cwd-dependent, so a static expectation like apps.man would be flaky)
    {
        extern int nexos_mcp_fs_remove(int fs, const char* name);
        // Idempotent across the two boot passes: a previous run may have left
        // the fixture behind.  Remove first, then open WITHOUT O_EXCL so this
        // assertion never depends on the earlier run's cleanup.  O_EXCL
        // semantics are already covered by the L2/L3 test above (/ost1.txt).
        nexos_mcp_fs_remove(0, "globt1.txt");
        OsFile* gf = 0;
        int gh = osapi_file_open_c("/globt1.txt", OFLAG_CREAT|OFLAG_WR, &gf);
        if (gh && gf){
            gf->buf[0] = 'x';
            gf->pos = 1; gf->size = 1; gf->dirty = 1;
            osapi_file_commit(gf);
            osapi_handle_close((h_t)gh, 0); gf->used = 0;
        } else {
            fails++; oser("  glob fixture create failed\n");
        }
        // Call the C-side implementation directly.  Routing through the
        // icall used clr_new_str(), which is 0 outside a CLR context, so the
        // first pass silently SKIPPED every glob assertion (false green)
        // while the second, in-CLR pass actually ran them and failed.
        static char got[4096];
        int r = osapi_fs_glob_c("/", "*.txt", got, sizeof(got));
        if (r < 0){ fails++; oser("  glob io error\n"); }
        else if (!got[0]){ fails++; oser("  glob *.txt empty\n"); }
        else {
            int found = 0;
            const char* p = got;
            while (*p){
                const char* q = p;
                while (*q && *q != '\n') q++;
                // The fixture is "globt1.txt" (9 chars); the old exact
                // `q - p == 8` check could never match it.  Compare the stem.
                if (q - p >= 6 && p[0]=='g' && p[1]=='l' && p[2]=='o' &&
                    p[3]=='b' && p[4]=='t' && p[5]=='1') { found = 1; break; }
                p = *q ? q + 1 : q;
            }
            if (!found){ fails++; oser("  glob *.txt missed created file\n"); }
        }
        nexos_mcp_fs_remove(0, "globt1.txt");
    }
    // 2) spawn of a nonexistent image must fail with NotFound
    int pid = osapi_proc_spawn_c("NoSuchImage.mex", g_active_token);
    if (pid || osapi_get_last_error() != ERR_NOTFOUND){
        fails++; oser("  spawn nonexistent did not fail cleanly\n");
    }
    // 3) wait/kill of a bad pid must fail with BadHandle
    if (osapi_ic_proc_wait(0) != -2 || osapi_get_last_error() != ERR_BADHANDLE){
        fails++; oser("  wait bad pid misbehaved\n");
    }
    if (osapi_ic_proc_kill(0) != 0){
        fails++; oser("  kill bad pid misbehaved\n");
    }
    oser("[OSAPI] proc/glob self-test fails=");
    oser_dec(fails);
    oser("\n");
    return fails;
}
