// knowledge_base.h - in-kernel verified-fact knowledge base for the AI engine.
//
// Design principle (per project owner):
//   "真理来源于实践；查不到的问题就用权威机构来源，不要臆测。"
//   Truth comes from practice; for questions we cannot verify, cite an
//   authoritative institution instead of guessing.
//
// Implementation: a small static array of curated, VERIFIED facts. Each entry
// carries an authoritative `source` (official site / registry / paper). At chat
// time we do lightweight keyword retrieval (RAG-style context injection): if a
// user question matches a fact, that fact is prepended to the model prompt so a
// small model does not hallucinate. If nothing matches, the system prompt tells
// the model to defer to authoritative sources rather than invent an answer.
//
// This is freestanding 64-bit C (compiled into kernel64 with -ffreestanding), so
// no libc string helpers are used; minimal case-insensitive matchers live in the
// .c file.

#ifndef KNOWLEDGE_BASE_H
#define KNOWLEDGE_BASE_H

#ifdef __cplusplus
extern "C" {
#endif

// A single verified fact. `keys` is a NULL-terminated array of trigger
// substrings (lowercase). `fact` is the verified statement; `source` names the
// authoritative institution it comes from.
typedef struct {
    const char*        topic;   // short label, e.g. "DeepSeek"
    const char* const* keys;    // NULL-terminated trigger keywords (lowercase)
    const char*        fact;    // verified fact text (Chinese)
    const char*        source;  // authoritative source (official / registry / paper)
} kb_entry_t;

// Returns 1 if a fact matches `question` (best keyword-score wins). On a hit,
// copies the fact into out_fact and the source into out_src (cap-limited), and
// sets *out_topic to the entry's topic. Returns 0 (and leaves buffers empty) on
// no match.
int kb_lookup(const char* question,
              char* out_fact, int fact_cap,
              char* out_src,  int src_cap,
              const char** out_topic);

// Builds an AI system prompt. If a fact matched, it injects the fact (RAG). If
// not, it instructs the model to defer to authoritative sources instead of
// guessing. Returns 1 when a fact was injected, 0 for the fallback form.
int kb_build_prompt(const char* question, char* out_prompt, int cap);

// Number of curated entries (for diagnostics).
int kb_count(void);

#ifdef __cplusplus
}
#endif

#endif // KNOWLEDGE_BASE_H
