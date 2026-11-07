#ifndef NEXOS_AI_KB_H
#define NEXOS_AI_KB_H
/* ai_kb.h -- appendable local knowledge base (increment entry).
 *
 * NEW FEATURE.  knowledge_base.c shipped a fixed table of curated facts that
 * only a rebuild could extend.  This module keeps the same "verified fact +
 * authoritative source" model but makes the table += able at runtime, with
 * a text format that survives in MKFS.
 *
 * WHY THIS MATTERS FOR A SMALL MODEL
 *   A 0.5B model hallucinates confidently.  Injecting one verified sentence
 *   with its source turns "I think..." into "According to <source>, ...".  The
 *   value is entirely in (a) the fact being true and (b) the source being
 *   named, so both are mandatory fields here -- an entry without a source is
 *   rejected rather than stored.
 *
 * STORAGE: fixed pools, no allocation.  64-bit only (32-bit image has no room).
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AI_KB_MAX_ENTRIES  48
#define AI_KB_KEYS_PER     6
#define AI_KB_TOPIC_LEN    32
#define AI_KB_FACT_LEN     320
#define AI_KB_SRC_LEN      96
#define AI_KB_KEY_LEN      24
#define AI_KB_POOL_BYTES   24576

typedef struct {
    char    topic[AI_KB_TOPIC_LEN];
    char    fact[AI_KB_FACT_LEN];
    char    source[AI_KB_SRC_LEN];
    char    keys[AI_KB_KEYS_PER][AI_KB_KEY_LEN];
    uint8_t n_keys;
    uint8_t builtin;      /* 1 = compiled-in, cannot be removed            */
} ai_kb_entry;

typedef struct {
    ai_kb_entry entries[AI_KB_MAX_ENTRIES];
    int         n;
    int         rejected;      /* entries refused (missing source, dup, ...) */
    int         dirty;
} ai_kb;

void ai_kb_reset(ai_kb* kb);

/* Add one entry.  Requires topic, fact AND source.  Keys may be empty (then
 * the topic is used as the only key).  Duplicate facts (same first 64 chars
 * of `fact`) are rejected.  Returns 0 on success, negative on rejection:
 *   -1 bad args, -2 missing source, -3 duplicate, -4 table full. */
int ai_kb_add(ai_kb* kb, const char* topic, const char* fact,
              const char* source, const char* keys_csv, int builtin);

/* Retrieve: case-insensitive keyword scoring over keys.  Returns 1 and fills
 * the outputs on a hit, 0 on no match.  `score_out` receives the best score
 * (number of matched keys) so a caller can require a minimum confidence. */
int ai_kb_lookup(const ai_kb* kb, const char* question,
                 char* fact_out, int fact_cap,
                 char* src_out, int src_cap,
                 char* topic_out, int topic_cap,
                 int* score_out);

/* Build a RAG prompt: preamble + (fact [+ source]) or the honest fallback
 * that tells the model to defer to authoritative sources.  Returns 1 when a
 * fact was injected, 0 for the fallback. */
int ai_kb_build_prompt(const ai_kb* kb, const char* question,
                       char* out, int cap);

/* Text format, one entry per line:
 *     topic|fact|source|key1,key2,key3
 * '#' starts a comment line.  Returns 0 on success. */
int ai_kb_serialize(const ai_kb* kb, char* buf, int cap);
int ai_kb_parse(ai_kb* kb, const char* text);

int ai_kb_count(const ai_kb* kb);
int ai_kb_rejected(const ai_kb* kb);

#ifdef __cplusplus
}
#endif
#endif
