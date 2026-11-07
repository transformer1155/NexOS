#ifndef NEXOS_PROC_ISOLATION_H
#define NEXOS_PROC_ISOLATION_H
/* proc_isolation.h -- Agent process isolation framework.
 *
 * Implements the four layers required by the isolation spec:
 *   1. hardware memory isolation  (private PML4, no identity map, CR0.WP)
 *   2. privilege isolation        (Ring 3 + syscall whitelist)
 *   3. VFS namespace isolation    (per-agent root_inode, read-only system mounts)
 *   4. communication isolation    (no sockets; everything through MCP)
 *
 * STATUS: framework only.  Nothing calls it yet, and it is deliberately NOT
 * linked into the build until a boot window is available to verify it.
 * Freestanding, no libc.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AGENT_MAX 8
#define AGENT_PT_POOL_PAGES 64

/* ---- 1. hardware memory isolation ---------------------------------- */
typedef struct AgentMem {
    uint64_t pml4_phys;          /* CR3 value for this agent            */
    uint64_t code_va, code_len;  /* RX mapping                          */
    uint64_t data_va, data_len;  /* RW mapping                          */
    uint64_t ring_va, ring_len;  /* MCP ring buffer, RW shared          */
    int      active;
} AgentMem;

/* Create a private address space for agent `slot`: allocates a PML4, maps
 * ONLY code(RX) / data(RW) / ring(RW) at the given virtual addresses, and
 * never installs an identity mapping.  Returns 0 on success. */
int  agent_mem_create(int slot, uint64_t code_va, uint64_t code_len,
                      uint64_t data_va, uint64_t data_len,
                      uint64_t ring_va, uint64_t ring_len);
void agent_mem_destroy(int slot);
/* Switch to the agent's CR3 (kernel must have saved its own). */
int  agent_mem_enter(int slot);
void agent_mem_leave(void);
/* 1 if `va` is mapped in the agent's own space (used by the fault handler
 * to decide whether a #PF is a genuine agent fault -> kill). */
int  agent_mem_va_mapped(int slot, uint64_t va);

/* ---- 2. privilege isolation --------------------------------------- */
#define AGENT_SYS_READ    3
#define AGENT_SYS_WRITE   4
#define AGENT_SYS_EXIT    1
#define AGENT_SYS_MMAP    9
#define AGENT_SYS_PTRACE  26

/* Returns 1 if the syscall is allowed for an agent.  Everything not in the
 * whitelist is -EPERM, including mmap (executable memory) and ptrace. */
int agent_syscall_allowed(int nr);
/* Called from the syscall entry when the caller is an agent. */
int32_t agent_syscall_gate(int nr, uint32_t a0, uint32_t a1, uint32_t a2);

/* ---- 3. VFS namespace isolation ------------------------------------ */
/* Per-agent sandbox root.  `root_inode_id` is the VFS inode the agent sees
 * as "/"; all path resolution is prefixed with it. */
typedef struct AgentVfs {
    int32_t root_inode_id;
    char    prefix[64];          /* e.g. "/sandbox/agent_01"  */
    int     mount_ro;            /* system mounts are read-only */
    int     active;
} AgentVfs;

int  agent_vfs_bind(int slot, const char* prefix);
void agent_vfs_unbind(int slot);
/* Resolve `path` for `slot`: returns a canonical path inside the sandbox,
 * or NULL if it escapes (.. / absolute outside).  Writes into out. */
const char* agent_vfs_resolve(int slot, const char* path, char* out, int cap);
/* Interception helpers: return -EPERM when the target is outside the
 * sandbox or the mount is read-only. */
int agent_vfs_check_write(int slot, const char* path);
int agent_vfs_check_unlink(int slot, const char* path);
int agent_vfs_check_rename(int slot, const char* from, const char* to);

/* ---- 4. communication isolation ----------------------------------- */
/* Agents may never open sockets. */
int agent_net_allowed(int slot);
/* Outbound payload scan before it leaves through MCP.  Returns 0 if clean,
 * -1 if it looks like secret material (caller must abort + revoke). */
int agent_mcp_scan(const char* payload, int len);

#ifdef __cplusplus
}
#endif
#endif
/* ====================================================================
 * ENTERPRISE SANDBOX (extends the four layers above)
 * ====================================================================
 *
 * Everything an untrusted agent -- or AI-GENERATED local code -- executes
 * runs inside a sandbox slot created here.  A slot owns:
 *   * its own PML4 (never shared with the kernel)
 *   * a page policy that REJECTS any W+X mapping at creation
 *   * a RAMFS root that backs writes (copy-on-write over a read-only base)
 *   * behaviour counters (Danger Table) that can kill it
 *
 * Invariant: an agent can never name a host path.  Paths resolve INSIDE the
 * sandbox root; there is no host directory a sandbox path can reach.
 */

/* ---- page permission policy ---------------------------------------- */
#define SANDBOX_PT_RX 0x001u    /* present | user                 */
#define SANDBOX_PT_RW 0x003u    /* present | user | writable      */
#define SANDBOX_PT_W  0x002u
#define SANDBOX_PT_X  0x004u

/* 1 when the flag combination is acceptable.  W+X is refused outright: a
 * writable executable page is exactly what self-modifying shellcode needs. */
int sandbox_pte_allowed(uint32_t flags);

/* ---- sandbox lifecycle --------------------------------------------- */
#define SANDBOX_MAX 4
#define SANDBOX_PATH_MAX 256
#define SANDBOX_TMP_QUOTA 262144   /* bytes of RAMFS per sandbox */

typedef struct Sandbox {
    int      slot;
    int      active;
    uint64_t pml4_phys;
    uint64_t ramfs_bytes;
    uint32_t writes_this_second;
    uint32_t mem_requests;
    uint32_t priv_attempts;
    uint32_t last_tick;
    uint32_t kill_reason;
    char     root[SANDBOX_PATH_MAX];
} Sandbox;

/* Create a sandbox: private PML4, RAMFS root, zeroed counters.
 * Returns slot index >= 0, or negative on failure. */
int  sandbox_create(const char* name);

/* Burn it down: release PML4 leaves, drop RAMFS contents and context.
 * Idempotent.  Call on normal completion AND on any fault: nothing survives,
 * so a crashed agent leaves no backdoor. */
void sandbox_destroy(int slot);

/* ---- VFS namespace with copy-on-write ------------------------------ */
/* Resolve path inside the sandbox.  Absolute paths are rebased; any component
 * equal to ".." is refused (no jail escape).  Returns 0 on success. */
int sandbox_path(int slot, const char* path, char* out, int cap);

/* A write attempt from inside the sandbox.
 *   returns 0  -> allowed, redirect holds the RAMFS destination
 *   returns -1 -> refused (quota, escape, sandbox gone)
 * The host filesystem is never the destination; the redirect always names the
 * sandbox RAMFS path. */
int sandbox_write_target(int slot, const char* path, char* redirect, int cap);

/* ---- network whitelist --------------------------------------------- */
/* Sockets are closed inside a sandbox.  Only mcp_fetch() may reach the
 * network; this decides whether a URL is acceptable. */
int sandbox_url_allowed(const char* url);

/* ---- Danger Table -------------------------------------------------- */
#define SANDBOX_KILL_NONE         0
#define SANDBOX_KILL_WRITE_FLOOD  1
#define SANDBOX_KILL_PRIV         2
#define SANDBOX_KILL_MEMORY       3

/* Feed one behaviour event.  Returns 0 to continue, or a SANDBOX_KILL_* code
 * meaning the caller must kill the process immediately. */
int  sandbox_note_write(int slot, uint32_t now_ms);
int  sandbox_note_memory(int slot, uint32_t bytes);
int  sandbox_note_priv(int slot);

int  sandbox_count_active(void);
const Sandbox* sandbox_get(int slot);
