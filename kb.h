// kb.h - dynamic, rule-driven knowledge base for the 32-bit NexOS kernel.
//
// Owner's principle (verbatim):
//   "真理来源于实践；如果不行的话就去网上查资料，需要查至少三篇都提到
//    这个观点才能进入知识库。"
//   Truth comes from practice. If practice cannot confirm a claim, search
//   the web -- but a claim only ENTERS the knowledge base after >= 3
//   independent web sources all state the same viewpoint.
//
// Implementation rules encoded here:
//   * A claim added via kb_add() starts as KB_CANDIDATE (unverified).
//   * kb_prove() records a PRACTICE outcome. Practice success (pass=1)
//     promotes it straight to KB_ACCEPTED  -> "真理来源于实践".
//   * kb_cite() records a host-assisted web result (the OS has no network
//     in a VM, so the agent/host performs the search and injects it).
//     When >= KB_WEB_MIN (3) distinct sources cite the SAME claim, it is
//     promoted to KB_ACCEPTED (web-corroborated).
//   * Fewer than 3 web sources => it stays KB_CANDIDATE (NOT knowledge yet).
//
// Freestanding C-compatible so it links into the 32-bit kernel.

#ifndef KB_H
#define KB_H

#ifdef __cplusplus
extern "C" {
#endif

// How a fact became accepted (or not yet).
enum kb_prov {
    KB_PROV_NONE    = 0,   // unverified
    KB_PROV_PRACTICE = 1,  // confirmed by running an experiment
    KB_PROV_WEB      = 2   // accepted via >=KB_WEB_MIN web sources
};

// Lifecycle status of a claim.
enum kb_status {
    KB_CANDIDATE = 0,      // proposed, not yet knowledge
    KB_ACCEPTED  = 1,      // entered the knowledge base
    KB_REJECTED  = 2       // contradicted / withdrawn
};

// Minimum number of independent web sources required to accept a claim
// that practice could not confirm. Owner rule: "至少三篇".
#define KB_WEB_MIN    3

#define KB_MAX_FACTS  48
#define KB_MAX_CITES  8
#define KB_STMT_LEN   200
#define KB_SRC_LEN    80
#define KB_EXC_LEN    140

// Add a candidate claim. Returns its 1-based id (>=1) or -1 on failure.
int kb_add(const char* statement);

// Record a practice (experiment) outcome for claim `idx` (1-based).
// pass=1 promotes to ACCEPTED immediately (truth from practice);
// pass=0 leaves it CANDIDATE (must then reach the web-source threshold).
// Returns the new status, or -1 if idx is invalid.
int kb_prove(int idx, int pass);

// Record one host-assisted web citation (source + excerpt mentioning the
// viewpoint). De-duplicated by source name. Reaching KB_WEB_MIN distinct
// sources promotes the claim to ACCEPTED. Returns new status, or -1.
int kb_cite(int idx, const char* source, const char* excerpt);

// Keyword retrieval over ACCEPTED facts (RAG grounding for the reasoner).
// Copies the best-matching accepted statement into `out`. Returns 1 on a
// hit, 0 on a miss (out left empty).
int kb_query(const char* question, char* out, int cap);

// Render the whole base (id, status, provenance, citation count, statement)
// into `out`. Returns characters written.
int kb_list(char* out, int cap);

// Render one fact (with its citations) into `out`. Returns chars written,
// or 0 if idx is invalid.
int kb_info(int idx, char* out, int cap);

// Seed curated, already-verified facts (practice-proven + web-corroborated).
// Idempotent.
void kb_init(void);

int  kb_fact_count(void);
int  kb_accepted(void);
const char* kb_status_name(int status);
const char* kb_prov_name(int prov);

#ifdef __cplusplus
}
#endif

#endif // KB_H
