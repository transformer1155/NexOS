#ifndef NEXOS_AI_USAGE_H
#define NEXOS_AI_USAGE_H
/* ai_usage.h -- per-provider usage ledger for the in-kernel AI.
 *
 * NEW FEATURE.  Today nobody can answer "how much AI did this machine do".
 * That is both a product gap (users want to see it) and the prerequisite for
 * ever charging money (you cannot bill what you do not measure).
 *
 * SCOPE, DELIBERATELY SMALL
 *   * Counters, not billing: requests, failures, characters in/out, ms.
 *   * Per provider, because local/opencode/deepseek have very different cost.
 *   * Fixed-size, no allocation, survives until reboot.
 *   * No persistence yet: writing to MKFS on every request would hammer the
 *     disk.  Serialise on demand (ai usage save) when that matters.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AI_USAGE_MAX_PROVIDERS 4

typedef struct {
    char     provider[16];
    uint32_t requests;
    uint32_t failures;
    uint64_t chars_in;
    uint64_t chars_out;
    uint64_t ms_total;
    uint64_t ms_max;
} ai_usage_row;

#define AI_QUOTA_MAX_AGENTS 8

typedef struct {
    char     agent[24];
    uint32_t daily_tokens;   /* cap; 0 = unlimited            */
    uint32_t used_tokens;
    uint32_t rejected;
    uint32_t period_day;
} ai_quota_row;

typedef struct {
    ai_usage_row rows[AI_USAGE_MAX_PROVIDERS];
    int          n;
    uint32_t     since_boot_requests;
    ai_quota_row quota[AI_QUOTA_MAX_AGENTS];
    int          n_quota;
} ai_usage;

void ai_usage_reset(ai_usage* u);

/* Record one completed call.  `ok` = 0 on failure.  Returns 0 on success. */
int  ai_usage_record(ai_usage* u, const char* provider,
                     uint32_t chars_in, uint32_t chars_out,
                     uint32_t ms, int ok);

/* Human-readable table.  Returns bytes written. */
int  ai_usage_describe(const ai_usage* u, char* buf, int cap);

/* Compact single line for a status bar: "local:12 opencode:3". */
int  ai_usage_summary(const ai_usage* u, char* buf, int cap);

/* Serialise / parse for ai_usage.cfg persistence (same kv style as ai_config).
 * Format: one line per provider: name=requests,failures,chars_in,chars_out,ms,ms_max
 * Returns bytes written (serialise) / 0 (parse). */
int  ai_usage_serialize(const ai_usage* u, char* buf, int cap);
int  ai_usage_parse(ai_usage* u, const char* text);

/* ---- ENTERPRISE QUOTA (per-agent daily token budget) ---------------- */

/* Attach a daily token budget to an agent id (tokens == 0 removes the cap).
 * Returns 0 on success, -1 bad args, -2 table full. */
int  ai_usage_quota_set(ai_usage* u, const char* agent, uint32_t tokens);

/* Call BEFORE dispatching.  Returns 1 allowed, 0 allowed and cap reached
 * after this call, -1 no such agent, -2 quota exhausted (MUST NOT send). */
int  ai_usage_quota_check(ai_usage* u, const char* agent,
                          uint32_t est_tokens, uint32_t today);

/* Call AFTER completion with the actual tokens; rolls the day over when
 * today differs.  Returns tokens remaining (0 when exhausted). */
uint32_t ai_usage_quota_charge(ai_usage* u, const char* agent,
                               uint32_t tokens, uint32_t today);

/* 1 when the agent has hit its cap (GUI banner). */
int  ai_usage_quota_exhausted(const ai_usage* u, const char* agent);

#ifdef __cplusplus
}
#endif
#endif
