#ifndef NEXOS_AI_REDACT_H
#define NEXOS_AI_REDACT_H
/* ai_redact.h -- outbound redaction gate for enterprise deployments.
 *
 * WHY THIS EXISTS
 *   In enterprise mode the knowledge base contains internal documents.  Those
 *   must never appear in an HTTP request to a public model.  The knowledge
 *   base itself is local-only, but the PROMPT built from it travels.
 *   This module is the single choke point every outbound payload passes
 *   through before net_https_post().
 *
 * TWO LAYERS, because they catch different mistakes
 *   1. PATTERN rules  -- credentials and absolute paths (sk-..., BEGIN KEY,
 *      /etc/passwd, private key blocks).  Catches accidents.
 *   2. CANARY rules   -- a caller registers a short unique token from a
 *      sensitive document (ai_redact_add_canary) and any outbound payload
 *      containing it is refused outright.  Catches 'the KB fact leaked into
 *      the prompt' regardless of how it was worded.
 *
 * Policy: REFUSE, do not silently scrub.  A silently-scrubbed prompt gives a
 * confidently wrong answer; a refusal gives an honest error the UI can show.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AI_REDACT_MAX_CANARY 8
#define AI_REDACT_CANARY_LEN 32

typedef enum {
    AI_REDACT_OK = 0,
    AI_REDACT_PATTERN = -1,   /* looks like a credential / sensitive path   */
    AI_REDACT_CANARY = -2     /* contains a protected snippet               */
    ,AI_REDACT_TOO_LONG = -3
} ai_redact_result;

void ai_redact_reset(void);

/* Register a canary token.  Typically the first 24-32 chars of a KB fact, or
 * a code identifier unique to the document.  Returns 0, or -1 when full. */
int  ai_redact_add_canary(const char* token);

/* Inspect an outbound payload.  `mode` comes from ai_config enterprise flag.
 * Non-enterprise mode still runs pattern rules (cheap, no false negatives on
 * obvious credentials).  Returns 0 when clean; negative result code otherwise.
 * `reason` receives a short static string for the UI / log. */
int  ai_redact_check(const char* payload, int len, int enterprise,
                     const char** reason);

/* Convenience wrapper used right before net_https_post(): returns 0 to send,
 * negative to refuse.  Never mutates the payload. */
int  ai_redact_gate(const char* payload, int len, int enterprise);

#ifdef __cplusplus
}
#endif
#endif
