// =====================================================================
//  ai_market.h  -  NexOS application market (Stage 0: offline/SFS registry)
// ---------------------------------------------------------------------
//  WHAT THIS IS
//    A display case plus one closed loop:
//        discover -> claim -> enable -> the AI can use the tool
//    Deliberately thin: no ratings, no search, no updates, no rollback.  A
//    static GitHub page (registry.json + apps/<id>/manifest.json + dist/*)
//    is the shop window; NexOS only has to read a registry and turn an entry
//    into a registered plugin (claim) and an allowed tool set (enable).
//
//  STAGES (agreed with the team)
//    Stage 0  THIS FILE.  registry.json lives in SFS.  Parse, browse, claim,
//             enable, sha256-verify local dist files.  Zero network, zero TLS,
//             fully testable offline.
//    Stage 1a A host/CI step pre-fetches registry + dist into SFS.  Still no
//             in-kernel TLS, so the demo never blocks on it.
//    Stage 1b net_https_get().  The kernel currently has NO https GET
//             (net.cpp:1907 "will fail later, no TLS"; net_https_post() is
//             POST-only).  Certificate policy for the MVP: pin an anchor with
//             net_tls_add_anchor().  Only this stage carries real risk.
//    Stage 2  market_list / market_claim / market_enable registered as
//             ai_tooluse skills so the model can drive the market itself.
//
//  SECURITY POSTURE (the registry is UNTRUSTED input)
//    * Every field is length-checked and copied with a bounded copy, so a
//      malformed or oversized registry can never overflow a buffer.
//    * sha256 is MANDATORY in Stage 0 -- not a Stage 1b nicety.  A market that
//      downloads code and enables it must verify what it got.  The digest uses
//      the already-linked tls_crypto sha256(), so the cost is zero new deps.
//    * Nothing is enabled implicitly: claim() registers a plugin, enable() is a
//      separate explicit step, and it maps each declared permission into the
//      ai_tooluse whitelist instead of granting a blanket capability.
//    * 64-bit only.  The 32-bit image has ~324 B spare, so an accidental 32-bit
//      compile is a hard error, not silent bloat.
// =====================================================================
#ifndef AI_MARKET_H
#define AI_MARKET_H

#include <stdint.h>

#if !defined(__x86_64__) && !defined(AI_MARKET_HOST_TEST)
#error "ai_market is 64-bit only: the 32-bit image has barely any spare space (build ai_market64.o)"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ---- limits (all sizes include the terminating NUL) ---- */
#define AI_MARKET_MAX_APPS   24
#define AI_MARKET_ID_MAX     48
#define AI_MARKET_NAME_MAX   48
#define AI_MARKET_VER_MAX    16
#define AI_MARKET_DIST_MAX   64
#define AI_MARKET_SHA_MAX    65   /* 64 hex + NUL */
#define AI_MARKET_PERM_MAX   24
#define AI_MARKET_PERMS_MAX   8
#define AI_MARKET_ERR_MAX    96
#define AI_MARKET_URL_MAX   160
#define AI_MARKET_TEXT_MAX 4096   /* registry / state document cap */

/* ---- SFS paths (Stage 0) ---- */
#define AI_MARKET_DIR        "/ai/market"
#define AI_MARKET_REG_PATH   "/ai/market/registry.json"   /* the shop window */
#define AI_MARKET_STATE_PATH "/ai/market/state.cfg"       /* claimed/enabled */
#define AI_MARKET_CACHE_DIR  "/ai/market/cache/"          /* fetched dist files */

/* One catalogue entry.  sha256 is the hex digest of the dist file. */
typedef struct {
    char id[AI_MARKET_ID_MAX];
    char name[AI_MARKET_NAME_MAX];
    char version[AI_MARKET_VER_MAX];
    char dist[AI_MARKET_DIST_MAX];
    char sha256[AI_MARKET_SHA_MAX];
    char perms[AI_MARKET_PERMS_MAX][AI_MARKET_PERM_MAX];
    int  perm_count;
    int  claimed;      /* registered into the plugin catalogue        */
    int  enabled;      /* permissions mapped into the tool whitelist  */
    int  sha_ok;       /* 1 verified, 0 not checked, -1 mismatch      */
    int  granted_count;/* permissions actually mapped on enable()     */
    int  skipped_count;/* mcp:/unknown perms: seen but NOT granted    */
} ai_market_app;

/* Glue supplied by the caller.  Keeping file/network/plugin access behind
 * callbacks is what lets this module be tested on the host with plain
 * buffers -- the same trick ai_tooluse uses for its completion backend. */
typedef struct {
    /* Read a whole SFS file.  Returns bytes read, or <0.  NULL means no FS. */
    int (*fs_read)(const char* path, char* out, int cap);
    /* Optional: size of an SFS file in bytes, or <0.  When present the verifier
     * can size its buffer exactly instead of guessing. */
    int (*fs_size)(const char* path);
    /* Optional heap hooks.  With alloc/free present, a dist larger than the
     * small static fallback is verified through a temporary allocation; without
     * them such a dist is REFUSED explicitly (see ai_market_claim, -10).  Never
     * silently truncated and never "verified" on partial bytes. */
    void* (*alloc)(int size);
    void (*free)(void* p);
    /* Write/overwrite an SFS file.  Returns 0, or <0. */
    int (*fs_write)(const char* path, const char* data, int len);
    /* Fetch a URL (Stage 1).  NULL means offline (Stage 0). */
    int (*fetch)(const char* url, char* out, int cap);
    /* Register a claimed plugin into ai_plugin.  Returns 0 or <0. */
    int (*plugin_register)(const char* id, const char* name);
    /* Allow one tool for the model: (short_name, app_id, tool).  0 or <0. */
    int (*tool_allow)(const char* name, const char* app_id, const char* tool);
    /* Load a claimed app so it becomes resident.  Returns 0 if the loader
     * accepted the dist, <0 otherwise.
     *
     * SECURITY: the market must NOT endorse an app id itself.  On this OS the
     * only thing that makes an app identity trustworthy is the MiniCLR loader
     * calling app_registry_note_resident() (nexos_ai.h:126-131), because that
     * is what mcp_dispatch_request() checks.  So this hook only *requests* a
     * load; the loader performs the endorsement.  A market entry can therefore
     * never forge the identity of an app the model talks to. */
    int (*load_app)(const char* dist_path, const char* app_id);
} ai_market_hooks;

typedef struct {
    ai_market_hooks hooks;
    ai_market_app   apps[AI_MARKET_MAX_APPS];
    int             app_count;
    char            source_url[AI_MARKET_URL_MAX];  /* Stage 1 registry URL */
    char            last_error[AI_MARKET_ERR_MAX];
    int             sha_checked;                    /* diagnostics          */
    int             sha_failed;
} ai_market_ctx;

/* ---- lifecycle ---- */
void ai_market_init(ai_market_ctx* m, const ai_market_hooks* hooks);

/* Parse a registry document.  Returns apps parsed (>=0) or <0.  Malformed
 * entries are skipped, never fatal: a broken shop window must not wedge the
 * shell.  Bounded input: at most AI_MARKET_MAX_APPS entries are kept. */
int  ai_market_load(ai_market_ctx* m, const char* json);

/* Stage 0 entry point: read the registry from AI_MARKET_REG_PATH through
 * fs_read, parse it, then restore claim/enable state from AI_MARKET_STATE_PATH.
 * Never touches the network.  Returns app count (>=0) or <0.
 *
 * WHY THE CALLER OWNS THE REGISTRY BUFFER: this module keeps no registry-sized
 * static storage on purpose.  Its earlier 24 KB of .bss pushed the 64-bit image
 * past HEAP_START ([PMM] FATAL at boot) and the fix was to make every sizable
 * buffer caller-supplied.  A registry larger than scratch_cap is REFUSED, never
 * truncated. */
int  ai_market_sync_buf(ai_market_ctx* m, char* scratch, int scratch_cap);

/* ---- browse ---- */
/* "id|name|version|claimed|enabled|sha" per line for the GUI/terminal.
 * Returns bytes written; always NUL-terminated when cap > 0. */
int  ai_market_list(const ai_market_ctx* m, char* buf, int cap);

/* Index of an app id, or -1. */
int  ai_market_find(const ai_market_ctx* m, const char* id);

/* ---- claim / enable ---- */
/* Verify the dist digest (when cached and fs_read is available), then register
 * the plugin.  Returns 0 ok; <0 on error.  A digest mismatch is a hard
 * failure (-4) and leaves the entry unclaimed. */
int  ai_market_claim(ai_market_ctx* m, const char* id);

/* Map the declared permissions into the tool whitelist / MCP and mark the
 * entry enabled.  Returns the number of permissions actually granted (>=0),
 * or <0 on error (unknown app -1, not claimed -5).  An unrecognised permission
 * grants nothing: the safe default is no capability, not a guess. */
int  ai_market_enable(ai_market_ctx* m, const char* id);

int  ai_market_disable(ai_market_ctx* m, const char* id);

/* ---- persistence ---- */
/* "claimed=<id>" / "enabled=<id>" lines: small, diffable, hand-editable. */
int  ai_market_state(const ai_market_ctx* m, char* buf, int cap);
int  ai_market_restore(ai_market_ctx* m, const char* text);

/* ---- helpers (exposed for unit tests) ---- */
void ai_market_sha256_hex(const void* data, int len, char out[AI_MARKET_SHA_MAX]);
int  ai_market_sha_equal(const char* a, const char* b);   /* 1 = equal */
const char* ai_market_error(const ai_market_ctx* m);

#ifdef __cplusplus
}
#endif
#endif /* AI_MARKET_H */
