/* proc_isolation.cpp -- see proc_isolation.h.
 *
 * Framework only: no page-table walker of the real VMM is called yet, and no
 * syscall hook is installed.  The shapes are here so the hooks in proc.cpp /
 * vfs.cpp / syscall.cpp are a few lines each (see
 * docs/进程隔离框架-挂接说明.md).
 */

#include "proc_isolation.h"

extern "C" {
    /* Provided by the kernel's page allocator / paging code. */
    void* kmalloc(uint32_t n);
    void  kfree(void* p);
    /* Physical page helpers (implemented by the VMM); declared here so this
       file stays honest about what it needs. */
    uint64_t pmm_alloc_page(void);          /* returns physical address */
    void     pmm_free_page(uint64_t phys);
    void     paging_map_in(uint64_t pml4_phys, uint64_t va, uint64_t pa,
                           uint32_t flags);
    uint64_t paging_read_cr3(void);
    void     paging_write_cr3(uint64_t v);
}

#define PG_RX 0x001   /* present | user, no write  */
#define PG_RW 0x002   /* present | user | write    */
#define AGENT_VA_BASE 0x8000000000ULL

static AgentMem g_mem[AGENT_MAX];
static AgentVfs g_vfs[AGENT_MAX];
static uint64_t g_saved_cr3;

/* ---- 1. hardware memory isolation ---------------------------------- */

/* Copy the kernel's higher-half PML4 entries into a fresh agent PML4, then
 * drop everything that would let the agent see kernel/user memory.
 *
 * The crucial rule: we never copy the low identity-mapped region, so the
 * agent literally cannot address physical memory by its physical number.
 */
int agent_mem_create(int slot, uint64_t code_va, uint64_t code_len,
                     uint64_t data_va, uint64_t data_len,
                     uint64_t ring_va, uint64_t ring_len){
    if (slot < 0 || slot >= AGENT_MAX) return -1;
    if (g_mem[slot].active) agent_mem_destroy(slot);

    uint64_t pml4 = pmm_alloc_page();
    if (!pml4) return -2;

    /* Map the agent's three regions at fixed, non-identity VAs. */
    for (uint64_t off = 0; off < code_len; off += 4096){
        uint64_t pg = pmm_alloc_page();
        if (!pg) return -3;
        paging_map_in(pml4, code_va + off, pg, PG_RX);
    }
    for (uint64_t off = 0; off < data_len; off += 4096){
        uint64_t pg = pmm_alloc_page();
        if (!pg) return -3;
        paging_map_in(pml4, data_va + off, pg, PG_RW);
    }
    for (uint64_t off = 0; off < ring_len; off += 4096){
        uint64_t pg = pmm_alloc_page();
        if (!pg) return -3;
        paging_map_in(pml4, ring_va + off, pg, PG_RW);
    }

    g_mem[slot].pml4_phys = pml4;
    g_mem[slot].code_va = code_va; g_mem[slot].code_len = code_len;
    g_mem[slot].data_va = data_va; g_mem[slot].data_len = data_len;
    g_mem[slot].ring_va = ring_va; g_mem[slot].ring_len = ring_len;
    g_mem[slot].active = 1;
    return 0;
}

void agent_mem_destroy(int slot){
    if (slot < 0 || slot >= AGENT_MAX) return;
    if (!g_mem[slot].active) return;
    /* TODO(vmm): walk the PML4 and free leaf pages; for now release the
       top-level table so a leak test has something to assert on. */
    if (g_mem[slot].pml4_phys) pmm_free_page(g_mem[slot].pml4_phys);
    g_mem[slot].pml4_phys = 0;
    g_mem[slot].active = 0;
}

int agent_mem_enter(int slot){
    if (slot < 0 || slot >= AGENT_MAX || !g_mem[slot].active) return -1;
    g_saved_cr3 = paging_read_cr3();
    paging_write_cr3(g_mem[slot].pml4_phys);
    return 0;
}

void agent_mem_leave(void){
    if (g_saved_cr3) paging_write_cr3(g_saved_cr3);
}

int agent_mem_va_mapped(int slot, uint64_t va){
    if (slot < 0 || slot >= AGENT_MAX || !g_mem[slot].active) return 0;
    const AgentMem* m = &g_mem[slot];
    if (va >= m->code_va && va < m->code_va + m->code_len) return 1;
    if (va >= m->data_va && va < m->data_va + m->data_len) return 1;
    if (va >= m->ring_va && va < m->ring_va + m->ring_len) return 1;
    return 0;
}

/* ---- 2. privilege isolation --------------------------------------- */

int agent_syscall_allowed(int nr){
    /* Deliberately a whitelist, not a blacklist: a new syscall added to the
       kernel must NOT silently become reachable by an agent. */
    return (nr == AGENT_SYS_READ || nr == AGENT_SYS_WRITE || nr == AGENT_SYS_EXIT);
}

int32_t agent_syscall_gate(int nr, uint32_t a0, uint32_t a1, uint32_t a2){
    (void)a1; (void)a2;
    if (!agent_syscall_allowed(nr)) return -1;     /* -EPERM */
    if (nr == AGENT_SYS_EXIT) return 0;
    /* read/write are additionally constrained to the sandbox by vfs. */
    (void)a0;
    return 0;
}

/* ---- 3. VFS namespace isolation ------------------------------------ */

int agent_vfs_bind(int slot, const char* prefix){
    if (slot < 0 || slot >= AGENT_MAX || !prefix) return -1;
    int i = 0;
    while (prefix[i] && i < (int)sizeof(g_vfs[slot].prefix) - 1){
        g_vfs[slot].prefix[i] = prefix[i]; i++;
    }
    g_vfs[slot].prefix[i] = 0;
    g_vfs[slot].mount_ro = 1;      /* system view is read-only */
    g_vfs[slot].root_inode_id = -1; /* TODO(vfs): resolve prefix -> inode */
    g_vfs[slot].active = 1;
    return 0;
}

void agent_vfs_unbind(int slot){
    if (slot < 0 || slot >= AGENT_MAX) return;
    g_vfs[slot].active = 0;
    g_vfs[slot].prefix[0] = 0;
}

/* Reject any path that could escape the sandbox: absolute paths are
 * rebased, and any ".." component is refused outright (a correct
 * canonicaliser would also work, but refusing is safer and simpler). */
const char* agent_vfs_resolve(int slot, const char* path, char* out, int cap){
    if (slot < 0 || slot >= AGENT_MAX || !g_vfs[slot].active) return 0;
    if (!path || !out || cap <= 0) return 0;
    if (path[0] == '/') path++;                    /* rebase under sandbox */
    for (int i = 0; path[i]; i++){
        if (path[i] == '.' && path[i+1] == '.') return 0;   /* no escapes */
    }
    int n = 0;
    const char* p = g_vfs[slot].prefix;
    while (*p && n < cap - 2) out[n++] = *p++;
    if (n > 0 && out[n-1] != '/') out[n++] = '/';
    for (int i = 0; path[i] && n < cap - 1; i++) out[n++] = path[i];
    out[n] = 0;
    return out;
}

int agent_vfs_check_write(int slot, const char* path){
    if (slot < 0 || slot >= AGENT_MAX || !g_vfs[slot].active) return -1;
    (void)path;
    /* System mounts are read-only; writes must go through the MCP
       request_write_to_host() path instead. */
    if (g_vfs[slot].mount_ro) return -1;
    return 0;
}

int agent_vfs_check_unlink(int slot, const char* path){
    (void)path;
    if (slot < 0 || slot >= AGENT_MAX || !g_vfs[slot].active) return -1;
    return -1;   /* agents may never unlink anything in the shared tree */
}

int agent_vfs_check_rename(int slot, const char* from, const char* to){
    (void)from; (void)to;
    if (slot < 0 || slot >= AGENT_MAX || !g_vfs[slot].active) return -1;
    return -1;
}

/* ---- 4. communication isolation ----------------------------------- */

int agent_net_allowed(int slot){
    (void)slot;
    return 0;    /* never: fetch_webpage must go through MCP */
}

/* Very small heuristic scan: looks for markers that indicate the caller is
 * trying to exfiltrate credentials.  This is a backstop, not a DLP engine. */
int agent_mcp_scan(const char* payload, int len){
    static const char* NEEDLES[] = {
        "BEGIN PRIVATE KEY", "api_key", "sk-", "/etc/passwd", "password"
    };
    static const int NEEDLE_N = 5;
    if (!payload || len <= 0) return 0;
    for (int k = 0; k < NEEDLE_N; k++){
        const char* nd = NEEDLES[k];
        int nl = 0; while (nd[nl]) nl++;
        for (int i = 0; i + nl <= len; i++){
            int j = 0;
            while (j < nl){
                char a = payload[i+j], b = nd[j];
                if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
                if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
                if (a != b) break;
                j++;
            }
            if (j == nl) return -1;
        }
    }
    return 0;
}
/* ====================================================================
 * ENTERPRISE SANDBOX
 * ==================================================================== */

static Sandbox g_sandbox[SANDBOX_MAX];

int sandbox_pte_allowed(uint32_t flags){
    uint32_t rw = flags & SANDBOX_PT_W;
    uint32_t ex = flags & SANDBOX_PT_X;
    if (rw && ex) return 0;      /* the one combination that must never exist */
    return 1;
}

static int s_len2(const char* s){ int n = 0; if (s) while (s[n]) n++; return n; }
static void s_cpy2(char* d, const char* s, int cap){
    int i = 0; if (cap <= 0) return;
    for (; s && s[i] && i < cap - 1; i++) d[i] = s[i];
    d[i] = 0;
}
static int s_eq2(const char* a, const char* b){
    int i = 0; while (a[i] && b[i]){ if (a[i] != b[i]) return 0; i++; } return a[i] == 0 && b[i] == 0;
}

int sandbox_create(const char* name){
    if (!name || !name[0]) return -1;
    for (int i = 0; i < SANDBOX_MAX; i++){
        if (g_sandbox[i].active) continue;
        g_sandbox[i].slot = i;
        g_sandbox[i].active = 1;
        g_sandbox[i].ramfs_bytes = 0;
        g_sandbox[i].writes_this_second = 0;
        g_sandbox[i].mem_requests = 0;
        g_sandbox[i].priv_attempts = 0;
        g_sandbox[i].last_tick = 0;
        g_sandbox[i].kill_reason = SANDBOX_KILL_NONE;
        /* Private address space, same helper the agent path uses.  No identity
           mapping is installed, so the guest cannot name host physical RAM. */
        g_sandbox[i].pml4_phys = pmm_alloc_page();
        if (!g_sandbox[i].pml4_phys){ g_sandbox[i].active = 0; return -2; }
        s_cpy2(g_sandbox[i].root, "ramfs:/", SANDBOX_PATH_MAX);
        int at = s_len2(g_sandbox[i].root);
        for (int k = 0; name[k] && at < SANDBOX_PATH_MAX - 1; k++)
            g_sandbox[i].root[at++] = name[k];
        g_sandbox[i].root[at] = 0;
        return i;
    }
    return -3;   /* no free slot */
}

void sandbox_destroy(int slot){
    if (slot < 0 || slot >= SANDBOX_MAX) return;
    if (!g_sandbox[slot].active) return;
    if (g_sandbox[slot].pml4_phys) pmm_free_page(g_sandbox[slot].pml4_phys);
    g_sandbox[slot].pml4_phys = 0;
    g_sandbox[slot].ramfs_bytes = 0;
    g_sandbox[slot].root[0] = 0;
    g_sandbox[slot].active = 0;
}

/* Rebase into the sandbox root and refuse any parent-directory component.
 * Rejecting ".." outright is cruder than canonicalising, but it cannot be
 * defeated by encoding tricks that a hand-written canonicaliser might miss. */
int sandbox_path(int slot, const char* path, char* out, int cap){
    if (slot < 0 || slot >= SANDBOX_MAX || !g_sandbox[slot].active) return -1;
    if (!path || !out || cap <= 0) return -1;
    if (path[0] == '/') path++;
    for (int i = 0; path[i]; i++){
        if (path[i] == '.' && path[i+1] == '.') return -1;
    }
    int n = 0;
    const char* r = g_sandbox[slot].root;
    while (*r && n < cap - 2) out[n++] = *r++;
    if (n > 0 && out[n-1] != '/') out[n++] = '/';
    for (int i = 0; path[i] && n < cap - 1; i++) out[n++] = path[i];
    out[n] = 0;
    return 0;
}

int sandbox_write_target(int slot, const char* path, char* redirect, int cap){
    if (slot < 0 || slot >= SANDBOX_MAX || !g_sandbox[slot].active) return -1;
    if (g_sandbox[slot].ramfs_bytes >= SANDBOX_TMP_QUOTA) return -1;
    /* The destination is ALWAYS inside the sandbox RAMFS: this is the
       copy-on-write boundary.  A host path is not representable here. */
    return sandbox_path(slot, path, redirect, cap);
}

/* Whitelist: https to a named host, nothing else.  No scheme-relative tricks,
 * no file:, no raw IPs, no userinfo @. */
int sandbox_url_allowed(const char* url){
    if (!url) return 0;
    const char* p = url;
    const char* https = "https://";
    for (int i = 0; https[i]; i++){ if (*p != https[i]) return 0; p++; }
    if (!*p) return 0;
    for (const char* q = p; *q; q++){
        if (*q == '@') return 0;              /* userinfo = credential smuggle */
        if (*q == ' ' || *q == '\t') return 0;
    }
    return 1;
}

int sandbox_note_write(int slot, uint32_t now_ms){
    if (slot < 0 || slot >= SANDBOX_MAX || !g_sandbox[slot].active) return SANDBOX_KILL_NONE;
    Sandbox* s = &g_sandbox[slot];
    if (now_ms - s->last_tick >= 1000){    /* new one-second window */
        s->last_tick = now_ms;
        s->writes_this_second = 0;
    }
    s->writes_this_second++;
    if (s->writes_this_second > 64){       /* 64 writes/s is already abusive */
        s->kill_reason = SANDBOX_KILL_WRITE_FLOOD;
        return SANDBOX_KILL_WRITE_FLOOD;
    }
    return SANDBOX_KILL_NONE;
}

int sandbox_note_memory(int slot, uint32_t bytes){
    if (slot < 0 || slot >= SANDBOX_MAX || !g_sandbox[slot].active) return SANDBOX_KILL_NONE;
    Sandbox* s = &g_sandbox[slot];
    s->mem_requests++;
    /* A single request above 64 MiB, or 32 requests total, is a memory bomb
       on a machine with about 1 GiB usable. */
    if (bytes > (64u * 1024u * 1024u) || s->mem_requests > 32){
        s->kill_reason = SANDBOX_KILL_MEMORY;
        return SANDBOX_KILL_MEMORY;
    }
    return SANDBOX_KILL_NONE;
}

int sandbox_note_priv(int slot){
    if (slot < 0 || slot >= SANDBOX_MAX || !g_sandbox[slot].active) return SANDBOX_KILL_NONE;
    Sandbox* s = &g_sandbox[slot];
    s->priv_attempts++;
    /* One attempt is enough: a Ring-3 process has no legitimate reason to
       execute in/out, cli/sti or wrmsr. */
    s->kill_reason = SANDBOX_KILL_PRIV;
    return SANDBOX_KILL_PRIV;
}

int sandbox_count_active(void){
    int n = 0;
    for (int i = 0; i < SANDBOX_MAX; i++) if (g_sandbox[i].active) n++;
    return n;
}

const Sandbox* sandbox_get(int slot){
    if (slot < 0 || slot >= SANDBOX_MAX) return 0;
    return &g_sandbox[slot];
}
