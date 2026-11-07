#ifndef NEXOS_AI_CONFIG_H
#define NEXOS_AI_CONFIG_H
/* ai_config.h -- AI provider configuration (NEW FEATURE).
 *
 * Replaces the hard-coded constants that used to be scattered across
 * nexos_ai.cpp / net_tls.cpp.  Everything about "which model, where, with
 * which credential" lives here, so callers ask questions instead of guessing.
 *
 * Design notes
 *   * Plain key=value text, one per line, so it is readable in the shell and
 *     diff-able in a bug report.  No JSON parser needed in the kernel.
 *   * The key is stored but NEVER printed in full: ai_config_describe()
 *     masks it.  `ai config` output must be safe to paste into a chat.
 *   * Absent file = sane defaults (local engine, no credential).  A broken
 *     file = defaults + a warning flag, never a boot failure.
 *   * Deliberately has no opinion about transport: it only says what to talk
 *     to.  net_https_post() / the host gateway decide how.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AI_PROVIDER_LOCAL   = 0,  /* built-in engine: gguf_infer / Markov    */
    AI_PROVIDER_OPENCODE = 1, /* host gateway, anonymous free quota      */
    AI_PROVIDER_DEEPSEEK = 2  /* direct HTTPS to api.deepseek.com        */
    ,AI_PROVIDER_CUSTOM  = 3
} ai_provider_t;

#define AI_CFG_MAX_KEY   192
#define AI_CFG_MAX_MODEL 64
#define AI_CFG_MAX_HOST  96

typedef struct {
    ai_provider_t provider;
    char key[AI_CFG_MAX_KEY];
    char model[AI_CFG_MAX_MODEL];
    char host[AI_CFG_MAX_HOST];
    int  max_tokens;
    int  temperature_x10;      /* 0..20  -> 0.0 .. 2.0 (integer, no floats) */
    int  dirty;                /* parsed with warnings                    */
    int  enterprise;           /* enterprise edition: stricter policy      */
    int  locked;               /* writes refused until admin unlocks       */
} ai_config_t;

/* Load from `text` (the contents of ai.cfg).  NULL/empty -> defaults.
 * Returns 0 on success (even if some lines were rejected), -1 only if `out`
 * is NULL.  Rejected lines set dirty=1. */
int ai_config_parse(ai_config_t* out, const char* text);

/* Serialise back to text.  Returns bytes written (excluding NUL), or -1.
 * Always writes a trailing newline so the file stays line-oriented. */
int ai_config_serialize(const ai_config_t* c, char* buf, int cap);

/* Apply one `name value` pair, as typed at the shell (`ai config key ...`).
 * Returns 0 on success, -1 unknown field, -2 value rejected. */
int ai_config_set(ai_config_t* c, const char* name, const char* value);

/* Human-readable summary with the credential masked.  Safe to display.
 * Returns bytes written. */
int ai_config_describe(const ai_config_t* c, char* buf, int cap);

/* Query helpers. */
const char* ai_provider_name(ai_provider_t p);
int         ai_provider_from_name(const char* s);
int         ai_config_ready(const ai_config_t* c);   /* 1 if usable as-is */

/* ---- enterprise write lock -----------------------------------------
 * In enterprise mode an ordinary user may READ the configuration but must
 * not change provider / host / key -- otherwise an employee can point the
 * machine at their own endpoint and exfiltrate company prompts, or burn the
 * company key.  Only the ring-0 human console may unlock.
 *
 * ai_config_set() returns -3 (locked) when a locked config is modified.
 */
void ai_config_set_enterprise(ai_config_t* c, int on);
int  ai_config_is_enterprise(const ai_config_t* c);
/* Called ONLY from the privileged console path.  Returns 0. */
int  ai_config_admin_unlock(ai_config_t* c);
int  ai_config_admin_lock(ai_config_t* c);
int  ai_config_is_locked(const ai_config_t* c);

#ifdef __cplusplus
}
#endif
#endif
