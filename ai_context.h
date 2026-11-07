#ifndef NEXOS_AI_CONTEXT_H
#define NEXOS_AI_CONTEXT_H
/* ai_context.h -- multi-turn conversation state for the in-kernel AI.
 *
 * NEW FEATURE.  ai_ask_text() was stateless: every question started from
 * nothing, so "那第二个文件呢？" had no referent.  This keeps a bounded
 * history and renders it into a prompt.
 *
 * BUDGET, NOT GROWTH
 *   A 0.5B model with a short context window does not benefit from a long
 *   transcript -- it degrades.  The policy here is: keep the most recent
 *   turns that fit a character budget, and when something has to be dropped,
 *   replace the oldest turns with a one-line summary marker rather than
 *   silently truncating mid-sentence (which the model reads as corruption).
 *
 * Freestanding, no libc, fixed storage -- no allocation in the kernel.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AI_CTX_MAX_TURNS   8
#define AI_CTX_TURN_CHARS  512
#define AI_CTX_BUDGET      3072   /* total characters of history rendered */

typedef enum { AI_ROLE_USER = 0, AI_ROLE_ASSISTANT = 1, AI_ROLE_TOOL = 2 } ai_role_t;

typedef struct {
    char          text[AI_CTX_TURN_CHARS];
    unsigned char role;      /* ai_role_t */
} ai_turn;

typedef struct {
    ai_turn turns[AI_CTX_MAX_TURNS];
    int     head;            /* index of the oldest turn               */
    int     count;           /* number of valid turns                  */
    int     dropped;         /* turns evicted since creation           */
    int     budget;          /* characters, 0 => AI_CTX_BUDGET         */
} ai_context;

void ai_context_reset(ai_context* c);

/* Append a turn.  When full, the oldest turn is evicted and `dropped` grows.
 * Returns 0 on success, -1 on bad args.  Over-long text is truncated on a
 * UTF-8 boundary. */
int  ai_context_add(ai_context* c, ai_role_t role, const char* text);

/* Render the whole conversation into `out` as a plain transcript.
 * Returns bytes written.  If turns were evicted, a marker line is included
 * so the model knows it is missing earlier context. */
int  ai_context_render(const ai_context* c, char* out, int cap);

/* Render only the most recent `n` turns (n<=0 => all). */
int  ai_context_render_recent(const ai_context* c, int n, char* out, int cap);

/* Escape hatch: drop everything older than `keep` turns. */
int  ai_context_trim(ai_context* c, int keep);

int  ai_context_count(const ai_context* c);
int  ai_context_dropped(const ai_context* c);

#ifdef __cplusplus
}
#endif
#endif
